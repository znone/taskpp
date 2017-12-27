.386
.MODEL FLAT
.CODE

?stack_top@this_task@task@@YAPAXXZ PROC PUBLIC
	mov eax, esp
	add eax, 4
	ret			
?stack_top@this_task@task@@YAPAXXZ ENDP

END
