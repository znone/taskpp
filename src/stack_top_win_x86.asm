.386
.MODEL FLAT
.CODE

?stack_top@this_task@taskpp@@YAPAXXZ PROC PUBLIC
	mov eax, esp
	add eax, 4
	ret			
?stack_top@this_task@taskpp@@YAPAXXZ ENDP

END
