// Offline PE preparation. Never patches in place or starts the game.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
export const supportedHashes=new Set([
  '622e106891adc7c80bac7e526ffa2eafe5f76c37bf6e5847b1f2406ebfb814c0',
  'c5e9503a426d93e5a867e6b76eff5123fa3066c5647328bfbdc52d1282f0c191' // earlier isolated kiosk-price fix
]);
export const hash=b=>crypto.createHash('sha256').update(b).digest('hex');
const check=(ok,message)=>{if(!ok) throw new Error(message);};
const align=(n,a)=>Math.ceil(n/a)*a;
export function checksum(b,offset) {
  let sum=0;
  for(let i=0;i<b.length;i+=2) {
    if(i===offset||i===offset+2) continue;
    sum+=b[i]+((b[i+1]??0)<<8);sum=(sum&65535)+(sum>>>16);
  }
  return (((sum&65535)+(sum>>>16))+b.length)>>>0;
}
export function layout(input) {
  check(supportedHashes.has(hash(input)),'Unsupported client SHA-256; only the supplied unpacked reference and known kiosk-price copy are accepted');
  const pe=input.readUInt32LE(60),opt=pe+24,count=input.readUInt16LE(pe+6);
  const table=opt+input.readUInt16LE(pe+20),header=table+40*count;
  const fileAlign=input.readUInt32LE(opt+36),sectionAlign=input.readUInt32LE(opt+32);
  const firstRaw=Math.min(...Array.from({length:count},(_,i)=>input.readUInt32LE(table+i*40+20)).filter(Boolean));
  check(align(header+40,fileAlign)<=firstRaw && input.subarray(header,header+40).every(b=>b===0),'No unused PE header space for bootstrap');
  return {pe,opt,count,table,header,fileAlign,sectionAlign,base:input.readUInt32LE(opt+28),
    rva:align(input.readUInt32LE(opt+56),sectionAlign),entry:input.readUInt32LE(opt+16),raw:align(input.length,fileAlign)};
}
export function extractPayload(object) {
  check(object.readUInt16LE(0)===0x14c,'Expected x86 bootstrap COFF');
  const sections=[];
  for(let i=0;i<object.readUInt16LE(2);i++) {
    const h=20+object.readUInt16LE(16)+40*i,n=object.readUInt32LE(h+16),start=object.readUInt32LE(h+20);
    if(!n) continue;
    const name=object.toString('ascii',h,h+8).replace(/\0.*$/,'');
    if(name.startsWith('.debug')) continue;
    check((name==='.text'||name==='.text$mn') && object.readUInt16LE(h+32)===0 && start+n<=object.length,'Unexpected bootstrap section/relocations');
    sections.push(object.subarray(start,start+n));
  }
  check(sections.length===1 && sections[0].length<4096,'Expected one bootstrap code section smaller than a page');
  return sections[0];
}
export function patchImage(input,payload) {
  const l=layout(input),output=Buffer.alloc(l.raw+align(payload.length,l.fileAlign));input.copy(output);
  check(payload.length>0 && payload.length<4096,'Invalid bootstrap size');
  output.write('.rchboot',l.header,'ascii');
  output.writeUInt32LE(payload.length,l.header+8);output.writeUInt32LE(l.rva,l.header+12);
  output.writeUInt32LE(output.length-l.raw,l.header+16);output.writeUInt32LE(l.raw,l.header+20);
  output.writeUInt32LE(0x60000020,l.header+36);
  output.writeUInt16LE(l.count+1,l.pe+6);
  output.writeUInt32LE(input.readUInt32LE(l.opt+4)+output.length-l.raw,l.opt+4);
  output.writeUInt32LE(l.rva,l.opt+16);
  output.writeUInt32LE(align(l.rva+payload.length,l.sectionAlign),l.opt+56);
  output.writeUInt32LE(align(l.header+40,l.fileAlign),l.opt+60);
  payload.copy(output,l.raw);output.writeUInt32LE(checksum(output,l.opt+64),l.opt+64);
  return output;
}
function main(args) {
  check(args.length===2,'Usage from x86 VS prompt: node tools/patch-client-autoload.mjs input.exe output.exe');
  const source=path.resolve(args[0]),target=path.resolve(args[1]);
  check(source.toLowerCase()!==target.toLowerCase(),'In-place patching is forbidden');
  check(!fs.existsSync(target)&&!fs.existsSync(target+'.json'),'Output already exists; choose a new path');
  const input=fs.readFileSync(source),l=layout(input);
  const object=path.join(root,'obj','Autoload','bootstrap.obj');fs.mkdirSync(path.dirname(object),{recursive:true});
  const hex=n=>'0'+n.toString(16)+'h';
  const result=spawnSync('ml.exe',['/nologo','/c','/coff',`/DBOOT_BASE=${hex(l.base+l.rva)}`,`/DORIGINAL_ENTRY=${hex(l.base+l.entry)}`,`/Fo${object}`,path.join(root,'tools','client-bootstrap.asm')],{encoding:'utf8'});
  check(!result.error&&result.status===0,`Assembler failed; run tools/build-autoload.cmd from this project. ${result.error?.message??result.stderr??''} ${result.stdout??''}`);
  const payload=extractPayload(fs.readFileSync(object)),output=patchImage(input,payload);
  const dll=path.join(root,'bin','Release','novaclient.dll');check(fs.existsSync(dll),'Build the client DLL first');
  const dllTarget=path.join(path.dirname(target),'novaclient.dll');
  check(!fs.existsSync(dllTarget),'Destination DLL already exists; choose a fresh output directory');
  const manifest={original:{path:source,sha256:hash(input),bytes:input.length},patched:{path:target,sha256:hash(output),bytes:output.length},
    dll:{path:dllTarget,sha256:hash(fs.readFileSync(dll))},entryRva:l.entry,bootstrapRva:l.rva,bootstrapBytes:payload.length,
    note:'Prepared offline. No game client launched. Requires the matching server AHK protocol update and normal game assets/launcher.'};
  fs.mkdirSync(path.dirname(target),{recursive:true});
  fs.writeFileSync(target,output,{flag:'wx'});fs.copyFileSync(dll,dllTarget,fs.constants.COPYFILE_EXCL);
  fs.writeFileSync(target+'.json',JSON.stringify(manifest,null,2)+'\n',{flag:'wx'});
  console.log(JSON.stringify(manifest,null,2));
}
if(process.argv[1]&&path.resolve(process.argv[1])===fileURLToPath(import.meta.url)) {
  try {main(process.argv.slice(2));} catch(error) {console.error(error.message);process.exitCode=1;}
}
