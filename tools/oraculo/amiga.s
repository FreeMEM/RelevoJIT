; SPDX-License-Identifier: MPL-2.0
;
; AmigaOS wrapper for a bench program of tools/oraculo/oraculo.py.
;
; The bench program ("core") is a flat binary that must run at $140000 in
; supervisor mode; its work area and results are at $15C000-$16FFFF. This
; wrapper takes that memory with AllocAbs(), copies the core there, runs it
; through Supervisor() with interrupts masked, and writes the results to the
; Shell's output as hex text:
;
;   ORACLE <bytes in hex>
;   <64 hex digits per line>
;
; Assembled by oraculo.py with vasm (Motorola syntax), the core included as
; CORE_FILE.

CORE        equ $140000
CORE_SIZE   equ $30000
RESPTR      equ $15C008
RESULTS     equ $160000

ExecBase    equ 4
LVOSupervisor   equ -30
LVOForbid       equ -132
LVOPermit       equ -138
LVOAllocMem     equ -198
LVOAllocAbs     equ -204
LVOFreeMem      equ -210
LVOCloseLibrary equ -414
LVOOpenLibrary  equ -552
LVOCacheClearU  equ -636
LVOWrite        equ -48
LVOOutput       equ -60

    section code,code

start:
    movem.l d2-d7/a2-a6,-(sp)
    move.l  ExecBase.w,a6
    lea     dosname(pc),a1
    moveq   #0,d0
    jsr     LVOOpenLibrary(a6)
    move.l  d0,a4                   ; a4 = DOSBase
    tst.l   d0
    beq     .nodos

    move.l  #CORE_SIZE,d0
    lea     CORE,a1
    jsr     LVOAllocAbs(a6)
    tst.l   d0
    bne.s   .got
    lea     msg_alloc(pc),a0
    bsr     print
    bra     .close
.got:
    ; copy the core and clear the rest of the block
    lea     core(pc),a0
    lea     CORE,a1
    move.l  #core_end-core,d0
.copy:
    move.b  (a0)+,(a1)+
    subq.l  #1,d0
    bne.s   .copy
    move.l  #CORE+CORE_SIZE,d0
.clear:
    clr.b   (a1)+
    cmp.l   a1,d0
    bne.s   .clear
    jsr     LVOCacheClearU(a6)

    jsr     LVOForbid(a6)
    lea     super(pc),a5
    jsr     LVOSupervisor(a6)
    jsr     LVOPermit(a6)

    ; results as hex text
    move.l  RESPTR,d7
    sub.l   #RESULTS,d7             ; d7 = bytes
    bmi     .bad
    cmp.l   #$10000,d7
    bhi     .bad
    move.l  d7,d0
    add.l   d0,d0                   ; 2 digits per byte
    move.l  d7,d1
    lsr.l   #5,d1
    add.l   d1,d0                   ; a newline per 32 bytes
    add.l   #64,d0
    move.l  d0,d6                   ; d6 = buffer size
    moveq   #0,d1
    jsr     LVOAllocMem(a6)
    tst.l   d0
    beq     .free
    move.l  d0,a3                   ; a3 = buffer
    lea     digits(pc),a2
    move.l  a3,a1
    lea     hdr(pc),a0
.h: move.b  (a0)+,(a1)+
    bne.s   .h
    subq.l  #1,a1
    move.l  d7,d0
    moveq   #7,d2                   ; 8 digits of the length
.hl:
    rol.l   #4,d0
    move.l  d0,d1
    and.w   #15,d1
    move.b  (a2,d1.w),(a1)+
    dbra    d2,.hl
    move.b  #10,(a1)+
    lea     RESULTS,a0
    move.l  d7,d3                   ; bytes left
    moveq   #0,d4                   ; bytes in this line
.byte:
    tst.l   d3
    beq.s   .end
    move.b  (a0)+,d0
    move.b  d0,d1
    lsr.b   #4,d1
    and.w   #15,d1
    move.b  (a2,d1.w),(a1)+
    and.w   #15,d0
    move.b  (a2,d0.w),(a1)+
    subq.l  #1,d3
    addq.l  #1,d4
    cmp.l   #32,d4
    bne.s   .byte
    move.b  #10,(a1)+
    moveq   #0,d4
    bra.s   .byte
.end:
    tst.l   d4
    beq.s   .out
    move.b  #10,(a1)+
.out:
    move.l  a1,d5
    sub.l   a3,d5                   ; d5 = text length
    exg     a4,a6
    jsr     LVOOutput(a6)
    move.l  d0,d1
    move.l  a3,d2
    move.l  d5,d3
    jsr     LVOWrite(a6)
    exg     a4,a6
    move.l  a3,a1
    move.l  d6,d0
    jsr     LVOFreeMem(a6)
    bra.s   .free
.bad:
    lea     msg_bad(pc),a0
    bsr     print
.free:
    lea     CORE,a1
    move.l  #CORE_SIZE,d0
    jsr     LVOFreeMem(a6)
.close:
    move.l  a4,a1
    jsr     LVOCloseLibrary(a6)
.nodos:
    movem.l (sp)+,d2-d7/a2-a6
    moveq   #0,d0
    rts

digits:
    dc.b    "0123456789abcdef"

; print: a0 = zero-terminated text; a4 = DOSBase, a6 = ExecBase
print:
    movem.l d2-d3/a6,-(sp)
    move.l  a0,d2
    moveq   #-1,d3
.len:
    addq.l  #1,d3
    tst.b   (a0)+
    bne.s   .len
    move.l  a4,a6
    jsr     LVOOutput(a6)
    move.l  d0,d1
    jsr     LVOWrite(a6)
    movem.l (sp)+,d2-d3/a6
    rts

; Supervisor() calls this in supervisor mode; it must end with RTE.
super:
    ori.w   #$0700,sr
    jsr     CORE
    rte

dosname:    dc.b "dos.library",0
hdr:        dc.b "ORACLE ",0
msg_alloc:  dc.b "ORACLE-ERROR AllocAbs $140000 failed",10,0
msg_bad:    dc.b "ORACLE-ERROR bad result pointer",10,0
    even
core:
    incbin  CORE_FILE
core_end:
    even
