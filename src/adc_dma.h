#ifndef ADC_DMA_H
#define ADC_DMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ADC_SAMPLE_RATE       400000u
#define ADC_SAMPLES_PER_BLOCK 1024u

/*
 * Wird aufgerufen, sobald ein vollständiger ADC-Block
 * zur Verarbeitung bereitsteht.
 */
typedef void (*adc_block_callback_t)(
	const uint16_t *samples,
	size_t sample_count,
	uint32_t sample_rate
);

struct adc_stream_stats {
	bool running;

	uint32_t produced_blocks;
	uint32_t sent_blocks;
	uint32_t dropped_blocks;
	uint32_t dma_errors;
	uint32_t blocks_remaining;
};

int adc_dma_init(adc_block_callback_t callback);

int adc_dma_start(uint32_t blocks);

void adc_dma_stop(void);

void adc_dma_get_stats(
	struct adc_stream_stats *stats
);

#endif /* ADC_DMA_H */
