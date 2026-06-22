#pragma once
#include <stdint.h>
#include <stdbool.h>

/* CDP1802 machine-cycle frequency */
#define CDP1802_CLOCK_HZ          1760000u
#define CDP1802_CLOCKS_PER_MCYCLE 8u
#define CDP1802_MCYCLES_PER_SEC   (CDP1802_CLOCK_HZ / CDP1802_CLOCKS_PER_MCYCLE)

/* NTSC frame: 262 lines × 14 machine cycles (~60 Hz) */
#define CDP1861_LINES_TOTAL        262
#define CDP1861_MCYCLES_PER_LINE   14
#define CDP1861_MCYCLES_PER_FRAME  (CDP1861_LINES_TOTAL * CDP1861_MCYCLES_PER_LINE)

/* PAL frame: 312 lines × 14 machine cycles (~50 Hz) */
#define CDP1861_PAL_LINES_TOTAL       312
#define CDP1861_PAL_MCYCLES_PER_FRAME (CDP1861_PAL_LINES_TOTAL * CDP1861_MCYCLES_PER_LINE)

#define CDP1861_DISPLAY_W          64
#define CDP1861_DISPLAY_H          128
#define CDP1861_BYTES_PER_LINE     (CDP1861_DISPLAY_W / 8)
#define CDP1861_FIRST_LINE         64
#define CDP1861_LAST_LINE          191

/* Ring-buffer capacity for pending DMA entries (power of 2, fits one full frame) */
#define CDP1802_DMA_QUEUE_CAP      2048u
#define CDP1802_DMA_QUEUE_MASK     (CDP1802_DMA_QUEUE_CAP - 1u)

typedef enum {
    CDP1802_S_EXECUTE,   /* reset-init lands here first */
    CDP1802_S_FETCH,
    CDP1802_S_DMA,
    CDP1802_S_INTERRUPT,
} Cdp1802CycleState;

typedef struct {
    bool   is_out;                    /* true = DMA-out (read mem→device) */
    void (*cb)(uint8_t *byte, void *ud);
    void  *ud;
} Cdp1802DmaEntry;

typedef struct Cdp1802 {
    /* Registers */
    uint8_t  D;
    uint8_t  DF;
    uint8_t  B;
    uint16_t R[16];
    uint8_t  P;           /* program-counter register selector */
    uint8_t  X;           /* data-pointer register selector */
    uint8_t  N;           /* low nibble of fetched instruction */
    uint8_t  I;           /* high nibble of fetched instruction */
    uint8_t  T;           /* saved X,P at interrupt time */
    uint8_t  IE;          /* interrupt enable */
    uint8_t  Q;           /* Q flip-flop */
    bool     EF[4];       /* external flag inputs EF1-EF4 */

    /* Execution state */
    Cdp1802CycleState state;
    int               exec_left;    /* execute-cycles remaining for current insn */
    bool              idle;
    bool              init_pending; /* true = waiting for first execute after reset */

    /* Pending external requests */
    bool dma_in_pending;
    bool dma_out_pending;
    bool irq_pending;
    struct { uint16_t count; void(*cb)(uint8_t*,void*); void *ud; } next_dma_in;
    struct { uint16_t count; void(*cb)(uint8_t*,void*); void *ud; } next_dma_out;

    /* DMA ring buffer */
    Cdp1802DmaEntry dma_queue[CDP1802_DMA_QUEUE_CAP];
    unsigned        dma_head, dma_count;

    /* Flat memory (caller-owned) */
    uint8_t *mem;
    uint32_t mem_mask;   /* typically 0xFFFF */
    uint16_t memory_addr; /* most recent memory address on the bus */

    /* I/O callbacks (all optional) */
    uint8_t (*io_in)(uint8_t port, void *ud);
    void    (*io_out)(uint8_t port, uint8_t val, void *ud);
    uint8_t (*mem_read)(uint16_t addr, void *ud);
    void    (*mem_write)(uint16_t addr, uint8_t val, void *ud);
    void    (*q_out)(uint8_t q, void *ud);
    void    (*on_sync)(void *ud);  /* called once per machine cycle */
    void    (*panic)(struct Cdp1802 *cpu, const char *reason, void *ud);
    void    *io_ud;
    void    *panic_ud;

    uint64_t cycle_count;
    uint64_t insn_count;
} Cdp1802;

void cdp1802_init(Cdp1802 *cpu, uint8_t *mem, uint32_t mem_size);
void cdp1802_reset(Cdp1802 *cpu);
void cdp1802_step(Cdp1802 *cpu);   /* one machine cycle */

/* Called by peripherals to queue requests */
void cdp1802_request_dma_out(Cdp1802 *cpu, uint16_t count,
                              void (*cb)(uint8_t*, void*), void *ud);
void cdp1802_request_dma_in(Cdp1802 *cpu, uint16_t count,
                             void (*cb)(uint8_t*, void*), void *ud);
void cdp1802_request_irq(Cdp1802 *cpu);
