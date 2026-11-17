"""Run the real generated x86 entry shim with fake Win32 APIs in Unicorn.
No game executable is launched. Requires the existing unicorn Python package.
"""
from pathlib import Path
import struct
import unittest
from unicorn import Uc,UC_ARCH_X86,UC_MODE_32,UC_HOOK_CODE
from unicorn.x86_const import *

ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'00_reference/rohanclient_unpacked.exe'
PATCHED=ROOT/'bin/Autoload/rohanclient_unpacked.exe'

def emulate(failure='',path=r'C:\Games\Rohan\rohanclient.exe'):
    image=PATCHED.read_bytes();pe=struct.unpack_from('<I',image,60)[0];opt=pe+24
    base=struct.unpack_from('<I',image,opt+28)[0]
    entry=base+struct.unpack_from('<I',image,opt+16)[0]
    size=struct.unpack_from('<I',image,opt+56)[0]
    original=SOURCE.read_bytes();old_entry=base+struct.unpack_from('<I',original,opt+16)[0]
    cpu=Uc(UC_ARCH_X86,UC_MODE_32);cpu.mem_map(base,size)
    cpu.mem_write(base,image[:struct.unpack_from('<I',image,opt+60)[0]])
    table=opt+struct.unpack_from('<H',image,pe+20)[0]
    for i in range(struct.unpack_from('<H',image,pe+6)[0]):
        rva,n,raw=struct.unpack_from('<III',image,table+40*i+12)
        cpu.mem_write(base+rva,image[raw:raw+n])
    cpu.mem_map(0x2000000,0x10000);cpu.mem_map(0x3000000,0x10000)
    iat=[0xc253fc,0xc2525c,0xc25214,0xc25220,0xc25610,0xc253d4]
    apis={0x2000000+i*0x100:address for i,address in enumerate(iat)}
    initializer=0x2000800
    for target,slot in apis.items():cpu.mem_write(slot,struct.pack('<I',target))
    registers={UC_X86_REG_EAX:0x11111111,UC_X86_REG_EBX:0x22222222,UC_X86_REG_ECX:0x33333333,
               UC_X86_REG_EDX:0x44444444,UC_X86_REG_ESI:0x55555555,UC_X86_REG_EDI:0x66666666,
               UC_X86_REG_EBP:0x77777777,UC_X86_REG_ESP:0x3008000,UC_X86_REG_EFLAGS:0x602}
    for register,value in registers.items():cpu.reg_write(register,value)
    called=[];loaded=[];stopped=[]
    def read_wide(address):
        result=bytearray()
        for i in range(260):
            pair=bytes(cpu.mem_read(address+2*i,2))
            if pair==b'\0\0':return result.decode('utf-16-le')
            result+=pair
        raise AssertionError('unterminated DLL path')
    def read_ascii(address):
        result=bytearray()
        for i in range(300):
            byte=bytes(cpu.mem_read(address+i,1))
            if byte==b'\0':return result.decode('ascii')
            result+=byte
        raise AssertionError('unterminated export')
    def hook(uc,address,length,user):
        if address not in apis and address!=initializer:return
        sp=uc.reg_read(UC_X86_REG_ESP)
        api=apis.get(address,initializer);called.append(api)
        ret=struct.unpack('<I',uc.mem_read(sp,4))[0]
        def args(count):return struct.unpack('<'+'I'*count,uc.mem_read(sp+4,4*count))
        value=1
        if api==0xc253fc:
            module,output,capacity=args(3);assert module==0 and capacity==260
            encoded=(path+'\0').encode('utf-16-le');uc.mem_write(output,encoded[:520])
            value=0 if failure=='path' else 260 if failure=='longpath' else len(path);count=3
        elif api==0xc2525c:
            loaded.append(read_wide(args(1)[0]));value=0 if failure=='load' else 0x10000000;count=1
        elif api==0xc25214:
            module,name=args(2);assert module==0x10000000
            assert read_ascii(name)=='NovaClient_Initialize'
            value=0 if failure=='export' else initializer;count=2
        elif api==initializer:
            assert args(1)==(0,);value=1114 if failure=='initialize' else 0;count=1
        elif api==0xc25220:count=1
        elif api==0xc25610:count=4
        elif api==0xc253d4:
            assert args(2)==(0xffffffff,1114);stopped.append(True);uc.emu_stop();return
        else:raise AssertionError('Unexpected API')
        uc.reg_write(UC_X86_REG_EAX,value);uc.reg_write(UC_X86_REG_ESP,sp+4+4*count);uc.reg_write(UC_X86_REG_EIP,ret)
    cpu.hook_add(UC_HOOK_CODE,hook);cpu.emu_start(entry,old_entry,count=10000)
    if not stopped:
        assert cpu.reg_read(UC_X86_REG_EIP)==old_entry
        for register,value in registers.items():assert cpu.reg_read(register)==value,(register,hex(cpu.reg_read(register)),hex(value))
    return called,loaded,stopped

class Bootstrap(unittest.TestCase):
    def test_success_restores_registers_flags_stack_and_entry(self):
        calls,loaded,stopped=emulate()
        self.assertFalse(stopped);self.assertEqual(loaded,[r'C:\Games\Rohan\novaclient.dll'])
        self.assertEqual(calls,[0xc253fc,0xc2525c,0xc25214,0x2000800])
    def test_unicode_folder(self):
        _,loaded,stopped=emulate(path='C:\\游戏\\rohanclient.exe')
        self.assertFalse(stopped);self.assertEqual(loaded,['C:\\游戏\\novaclient.dll'])
    def test_failures_never_enter_client(self):
        for failure in ['path','longpath','load','export','initialize']:
            calls,_,stopped=emulate(failure)
            self.assertTrue(stopped,failure);self.assertEqual(calls[-1],0xc253d4)
    def test_folder_too_long(self):
        _,_,stopped=emulate(path='C:\\'+'a'*244+'\\x.exe')
        self.assertTrue(stopped)
    def test_missing_separator(self):
        _,_,stopped=emulate(path='rohanclient.exe');self.assertTrue(stopped)

if __name__=='__main__':unittest.main()
