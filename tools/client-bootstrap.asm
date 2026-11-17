; Runs before the reference client's original entry, outside loader lock.
; No relocated operands. BOOT_BASE and ORIGINAL_ENTRY come from the PE patcher.
.386
.model flat
option casemap:none
.code
bootstrap_start:
    pushfd
    pushad
    cld
    sub esp, 544
    mov ebx, esp
    push 260
    push ebx
    push 0
    call dword ptr ds:[00C253FCh] ; GetModuleFileNameW
    test eax,eax
    jz fail
    cmp eax,260
    jae fail
find_separator:
    dec eax
    js fail
    cmp word ptr [ebx+eax*2],005Ch
    jne find_separator
    inc eax
    cmp eax,245
    ja fail
    lea edi,[ebx+eax*2]
    mov esi,BOOT_BASE+(dll_name-bootstrap_start)
    mov ecx,15
    rep movsw
    push ebx
    call dword ptr ds:[00C2525Ch] ; LoadLibraryW absolute sibling DLL path
    test eax,eax
    jz fail
    push BOOT_BASE+(initialize_name-bootstrap_start)
    push eax
    call dword ptr ds:[00C25214h] ; GetProcAddress
    test eax,eax
    jz fail
    push 0
    call eax
    test eax,eax
    jnz fail
    add esp,544
    popad
    popfd
    db 0E9h
    dd ORIGINAL_ENTRY-(BOOT_BASE+($-bootstrap_start)+4)
fail:
    push BOOT_BASE+(error_text-bootstrap_start)
    call dword ptr ds:[00C25220h] ; OutputDebugStringA
    push 10h
    push BOOT_BASE+(caption-bootstrap_start)
    push BOOT_BASE+(error_text-bootstrap_start)
    push 0
    call dword ptr ds:[00C25610h] ; MessageBoxA
    push 1114
    push -1
    call dword ptr ds:[00C253D4h] ; TerminateProcess, no detach on partial startup
stopped:
    jmp stopped
dll_name dw 'n','o','v','a','c','l','i','e','n','t','.','d','l','l',0
initialize_name db 'NovaClient_Initialize',0
caption db 'Nova Client',0
error_text db 'Nova Client could not start. Put the matching novaclient.dll beside this client and check novaclient.log. The client has been stopped.',0
end
