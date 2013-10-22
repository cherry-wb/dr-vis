.text
	.globl main

foo:
	xorl %eax,%eax
	inc %eax
	inc %eax
	ret
	

main:
	xorl %ebx,%ebx
	inc %ebx
	inc %ebx
	call foo
	ret
