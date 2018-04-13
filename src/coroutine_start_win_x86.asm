.386
.MODEL FLAT, C

_SEH_prolog4 PROTO C : ptr dword, : dword

.CODE

CoroutineStart PROC PUBLIC, lpAddress : PTR DWORD, lpThis : PTR DWORD
	assume fs:nothing	; reset exception handler
	mov eax, -1
	mov fs:[0], eax
	assume fs:error
	push 0CH
	push 75FAC040h
	call _SEH_prolog4
	add ebp, 4			; call coroutine
	mov ecx, lpThis
	mov eax, lpAddress
	call eax 
	ret
CoroutineStart ENDP

END
