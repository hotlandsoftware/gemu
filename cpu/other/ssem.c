/* SSEM - the Manchester Baby's processor; see ssem.h. */
#include "ssem.h"
#include <stdio.h>

void ssem_obey(Ssem *m, uint32_t insn) {
    uint32_t s = m->store[SSEM_LINE(insn)];
    switch ((SsemFunc)SSEM_FUNC(insn)) {
    case SSEM_JMP:  m->ci = s; break;
    case SSEM_JRP:  m->ci += s; break;
    case SSEM_LDN:  m->a = 0u - s; break;
    case SSEM_STO:  m->store[SSEM_LINE(insn)] = m->a; break;
    case SSEM_SUB:
    case SSEM_SUB5: m->a -= s; break;
    case SSEM_CMP:  if ((int32_t)m->a < 0) m->ci++; break;
    case SSEM_STP:  m->stopped = true; break;
    }
    m->steps++;
}

bool ssem_step(Ssem *m) {
    if (m->stopped) return false;
    m->ci++;
    m->pi = m->store[SSEM_LINE(m->ci)];
    ssem_obey(m, m->pi);
    return !m->stopped;
}

const char *ssem_mnemonic(unsigned func) {
    static const char *const names[8] = { "JMP", "JRP", "LDN", "STO", "SUB", "SUB", "CMP", "STP" };
    return names[func & 7];
}

void ssem_disasm(uint32_t insn, char *buf, size_t len) {
    unsigned f = SSEM_FUNC(insn);
    if (f == SSEM_CMP || f == SSEM_STP) snprintf(buf, len, "%s", ssem_mnemonic(f));
    else snprintf(buf, len, "%s %u", ssem_mnemonic(f), SSEM_LINE(insn));
}
