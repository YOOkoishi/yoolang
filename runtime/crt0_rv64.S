  .text
  .align 2
  .globl _start
_start:
  call main
  li a7, 93
  ecall
