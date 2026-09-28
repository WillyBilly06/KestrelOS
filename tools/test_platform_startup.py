#!/usr/bin/env python3
"""Execute the firmware PS/2 decision and actual local APIC setup writes."""
import re
from test_gpu_stable_candidate import ROOT, function, run_test


def main():
    acpi = (ROOT/'kernel/acpi.c').read_text()
    apic = (ROOT/'kernel/apic.c').read_text()
    cpu = (ROOT/'kernel/cpu.h').read_text()
    header = (ROOT/'kernel/acpi.h').read_text()
    record = header[header.index('typedef struct'):header.index('} acpi_sdt_t;')+len('} acpi_sdt_t;')]
    # Both PS/2 entry points consult firmware before touching shared ports.
    for path, name in (('input.c','input_init'),('mouse.c','mouse_init')):
        body = function((ROOT/'kernel'/path).read_text(),name)
        decision = body.index('if (!acpi_has_8042())')
        assert 'return;' in body[decision:body.index('}',decision)]
        io = re.search(r'\b(ps2_command|inb|outb|wait_write|irq_save)\(', body)
        assert io and decision < io.start()
    # Never write a vector-zero timer entry, even in calibration/stop paths.
    assert 'lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);' not in apic
    init = function(apic,'apic_init')
    setup = init[init.index('    lapic_write(LAPIC_LVT_ERROR, LVT_MASKED'):
                 init.index('    kinfo("apic", "local APIC id')]
    code = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef uint64_t u64;
'''+record+r'''
static unsigned char table[256];static acpi_sdt_t *found;
static acpi_sdt_t *acpi_find_table(const char *sig){assert(!strcmp(sig,"FACP"));return found;}
'''+function(acpi,'checksum_ok')+'\n'+function(acpi,'acpi_has_8042')+'\n'
    for src,pattern in ((apic,r'^#define (?:LAPIC_[A-Z_0-9]+|LVT_MASKED)\s+[^\n]+'),
                        (cpu,r'^#define VEC_[A-Z_0-9]+\s+[^\n]+')):
        code+='\n'.join(re.findall(pattern,src,re.M))+'\n'
    code+=r'''
static u32 regs[256],writes,esr_reads;
static bool error_handler,spurious_handler;
static void apic_error_isr(void){}
static void apic_spurious_isr(void){}
static void irq_install(int vector,void (*handler)(void),void *ctx){
    assert(!ctx);
    if(vector==VEC_APIC_ERROR){assert(handler==apic_error_isr);error_handler=true;}
    else{assert(vector==VEC_SPURIOUS&&handler==apic_spurious_isr);spurious_handler=true;}
}
static void lapic_write(u32 reg,u32 value){
    if(!writes)assert(reg==LAPIC_LVT_ERROR&&value==(LVT_MASKED|VEC_APIC_ERROR));
    if(reg==LAPIC_LVT_TIMER||reg==LAPIC_LVT_LINT0||reg==LAPIC_LVT_ERROR)
        assert((value&255)>=16);
    if(reg==LAPIC_LVT_TIMER||reg==LAPIC_LVT_LINT0)assert(value&LVT_MASKED);
    if(reg==LAPIC_LVT_ERROR&&!(value&LVT_MASKED))assert(error_handler&&esr_reads);
    if(reg==LAPIC_SPURIOUS)assert(spurious_handler);
    regs[reg/4]=value;writes++;
}
static u32 lapic_read(u32 reg){if(reg==LAPIC_ESR)esr_reads++;return regs[reg/4];}
static void setup(void){
'''+setup+r'''
}
static void checksum(void){
    found->checksum=0;u8 sum=0;for(unsigned i=0;i<found->length;i++)sum+=table[i];
    found->checksum=(u8)-sum;
}
int main(void){
    assert(acpi_has_8042());
    found=(acpi_sdt_t *)table;memcpy(found->signature,"FACP",4);
    for(unsigned rev=0;rev<7;rev++)for(unsigned len=36;len<=256;len++)
        for(unsigned flags=0;flags<4;flags++){
            found->revision=rev;found->length=len;table[109]=flags;checksum();
            bool expect=rev<3||len<111||(flags&2);
            assert(acpi_has_8042()==expect);
            found->checksum++;assert(acpi_has_8042()); /* corrupt table is unknown */
        }
    setup();assert(regs[LAPIC_LVT_ERROR/4]==VEC_APIC_ERROR);
    assert(regs[LAPIC_LVT_TIMER/4]==(LVT_MASKED|VEC_APIC_TIMER));
    assert(regs[LAPIC_LVT_LINT0/4]==(LVT_MASKED|VEC_IRQ_BASE));
    puts("PLATFORM_STARTUP_PASS 6188 FADT combinations + corrupt tables, legal masked APIC vectors and handler-before-unmask order");
}
'''
    run_test(code,'platform_startup')


if __name__=='__main__':
    main()
