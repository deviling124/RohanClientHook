// Refresh only this project's generated DLL/manifest; never changes game code.
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {hash} from './patch-client-autoload.mjs';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const exe=path.join(root,'bin','Autoload','rohanclient_unpacked.exe');
const manifestPath=exe+'.json';
if(fs.existsSync(manifestPath)) {
  const manifest=JSON.parse(fs.readFileSync(manifestPath,'utf8'));
  if(!fs.existsSync(exe)||hash(fs.readFileSync(exe))!==manifest.patched.sha256)
    throw new Error('Generated autoload client changed; prepare a fresh output before syncing the DLL');
  const source=path.join(root,'bin','Release','novaclient.dll'),target=path.join(path.dirname(exe),'novaclient.dll');
  fs.copyFileSync(source,target);
  manifest.dll={path:target,sha256:hash(fs.readFileSync(source))};
  fs.writeFileSync(manifestPath,JSON.stringify(manifest,null,2)+'\n');
  console.log('Updated generated autoload novaclient.dll and manifest.');
}
