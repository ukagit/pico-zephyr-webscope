#include "adc_dma.h"

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/dt-bindings/dma/rpi-pico-dma-common.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <hardware/adc.h>
#include <hardware/regs/dreq.h>
#include <hardware/structs/adc.h>

#define RING_BLOCKS 8
#define ADC_GPIO     26u
#define ADC_INPUT    0u
#define ADC_CLOCK_HZ 48000000.0f

enum block_state {
	BLOCK_FREE = 0,
	BLOCK_DMA,
	BLOCK_READY,
	BLOCK_TX,
};

static const struct device *const dma_dev =
	DEVICE_DT_GET(DT_NODELABEL(dma));

static uint16_t sample_ring[RING_BLOCKS]
			   [ADC_SAMPLES_PER_BLOCK]
			   __aligned(4);

static atomic_t block_states[RING_BLOCKS];

static uint8_t ready_queue[RING_BLOCKS];
static uint8_t ready_write;
static uint8_t ready_read;

K_SEM_DEFINE(ready_sem, 0, RING_BLOCKS);

static struct k_spinlock queue_lock;
static struct k_work_delayable start_work;

static int dma_channel = -1;
static uint8_t dma_block;
static bool initialized;

static adc_block_callback_t block_callback;

static struct adc_stream_stats stream_stats;


/*
 * Einen freien Ringpuffer für den nächsten DMA-Transfer suchen.
 *
 * atomic_cas() setzt den Zustand nur dann von BLOCK_FREE
 * auf BLOCK_DMA, wenn der Block tatsächlich noch frei ist.
 */
static int find_free_block(void)
{
	for (int i = 0; i < RING_BLOCKS; ++i) {
		if (atomic_cas(&block_states[i],
			       BLOCK_FREE,
			       BLOCK_DMA)) {
			return i;
		}
	}

	return -1;
}


/*
 * Prüfen, ob alle Ringpuffer wieder freigegeben wurden.
 */
static bool all_blocks_free(void)
{
	for (int i = 0; i < RING_BLOCKS; ++i) {
		if (atomic_get(&block_states[i]) !=
		    BLOCK_FREE) {
			return false;
		}
	}

	return true;
}


/*
 * Einen fertig aufgenommenen Block in die Warteschlange
 * für den Ausgabethread einfügen.
 *
 * Diese Funktion wird aus dem DMA-Callback aufgerufen.
 */
static void queue_ready_block(uint8_t block)
{
	k_spinlock_key_t key =
		k_spin_lock(&queue_lock);

	ready_queue[ready_write] = block;

	ready_write =
		(ready_write + 1u) % RING_BLOCKS;

	atomic_set(&block_states[block],
		   BLOCK_READY);

	k_spin_unlock(&queue_lock, key);

	k_sem_give(&ready_sem);
}


/*
 * Aufnahme beenden.
 *
 * Bereits fertige Blöcke bleiben in der Warteschlange und
 * werden noch vom adc_tx_thread verarbeitet.
 */
static void finish_capture(void)
{
	adc_run(false);
	adc_fifo_drain();

	stream_stats.running = false;
}


/*
 * DMA-Interrupt-Callback.
 *
 * Der gerade gefüllte Block wird freigegeben und der nächste
 * DMA-Transfer wird vorbereitet.
 */
static void dma_done(const struct device *dev,
		     void *user_data,
		     uint32_t channel,
		     int status)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);
	ARG_UNUSED(channel);

	uint8_t completed = dma_block;

	stream_stats.produced_blocks++;

	if (status != 0) {
		stream_stats.dma_errors++;
	}

	queue_ready_block(completed);

	if (stream_stats.blocks_remaining > 0) {
		stream_stats.blocks_remaining--;
	}

	/*
	 * Letzter gewünschter Block oder DMA-Fehler:
	 * keine weitere Aufnahme starten.
	 */
	if (stream_stats.blocks_remaining == 0 ||
	    status != 0) {
		finish_capture();
		return;
	}

	/*
	 * Einen freien Block für den nächsten DMA-Transfer suchen.
	 */
	int next = find_free_block();

	if (next < 0) {
		stream_stats.dropped_blocks++;
		finish_capture();
		return;
	}

	dma_block = (uint8_t)next;

	/*
	 * DMA-Zieladresse auf den neuen Ringpuffer umstellen.
	 */
	int ret = dma_reload(
		dma_dev,
		dma_channel,
		(uint32_t)(uintptr_t)&adc_hw->fifo,
		(uint32_t)(uintptr_t)
			sample_ring[dma_block],
		sizeof(sample_ring[dma_block])
	);

	if (ret != 0) {
		stream_stats.dma_errors++;

		atomic_set(
			&block_states[dma_block],
			BLOCK_FREE
		);

		finish_capture();
	}
}


/*
 * Der DMA wird etwas verzögert gestartet.
 *
 * Dadurch kann der Shell-Befehl seine Antwort und den Prompt
 * vollständig ausgeben, bevor die Aufnahme beginnt.
 */
static void start_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!stream_stats.running) {
		return;
	}

	int ret = dma_start(dma_dev,
			    dma_channel);

	if (ret != 0) {
		stream_stats.dma_errors++;
		stream_stats.running = false;

		atomic_set(
			&block_states[dma_block],
			BLOCK_FREE
		);

		return;
	}

	adc_run(true);
}


/*
 * Ausgabethread.
 *
 * Dieser Thread bekommt fertige ADC-Blöcke und übergibt sie
 * an die registrierte Callback-Funktion.
 *
 * Die Callback-Funktion wird später die Daten verkleinern und
 * zum Browser-Websocket beziehungsweise SSE-Ausgang schicken.
 */
static void tx_thread(void *arg1,
		      void *arg2,
		      void *arg3)
{
	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	for (;;) {
		k_sem_take(&ready_sem,
			   K_FOREVER);

		k_spinlock_key_t key =
			k_spin_lock(&queue_lock);

		uint8_t block =
			ready_queue[ready_read];

		ready_read =
			(ready_read + 1u) %
			RING_BLOCKS;

		atomic_set(
			&block_states[block],
			BLOCK_TX
		);

		k_spin_unlock(&queue_lock, key);

		/*
		 * Die Callback-Funktion wird nicht im DMA-Interrupt,
		 * sondern in diesem normalen Thread ausgeführt.
		 */
		if (block_callback != NULL) {
			block_callback(
				sample_ring[block],
				ADC_SAMPLES_PER_BLOCK,
				ADC_SAMPLE_RATE
			);
		}

		stream_stats.sent_blocks++;

		/*
		 * Der Block kann jetzt wieder vom DMA verwendet werden.
		 */
		atomic_set(
			&block_states[block],
			BLOCK_FREE
		);
	}
}


/*
 * Relativ niedrige Thread-Priorität, damit Netzwerk, WiFi
 * und System-Threads genügend Rechenzeit bekommen.
 */
K_THREAD_DEFINE(
	adc_tx_thread,
	3072,
	tx_thread,
	NULL,
	NULL,
	NULL,
	14,
	0,
	0
);


int adc_dma_init(adc_block_callback_t callback)
{
	if (!device_is_ready(dma_dev)) {
		return -ENODEV;
	}

	block_callback = callback;

	for (int i = 0; i < RING_BLOCKS; ++i) {
		atomic_set(
			&block_states[i],
			BLOCK_FREE
		);
	}

	ready_read = 0;
	ready_write = 0;

	k_work_init_delayable(
		&start_work,
		start_work_handler
	);

	dma_channel =
		dma_request_channel(dma_dev,
				    NULL);

	if (dma_channel < 0) {
		return dma_channel;
	}

	/*
	 * RP2350 ADC initialisieren.
	 *
	 * GP26 entspricht ADC-Eingang 0.
	 */
	adc_init();
	adc_gpio_init(ADC_GPIO);
	adc_select_input(ADC_INPUT);
	adc_set_round_robin(0);

	/*
	 * ADC-Takt: 48 MHz
	 *
	 * Gewünschte Abtastrate: 400 kSamples/s
	 *
	 * 48 MHz / 400 kHz = 120 Takte
	 *
	 * adc_set_clkdiv() verwendet intern 1 + divider.
	 */
	adc_set_clkdiv(
		(ADC_CLOCK_HZ /
		 (float)ADC_SAMPLE_RATE) - 1.0f
	);

	/*
	 * ADC-FIFO:
	 *
	 * FIFO aktivieren
	 * DMA-Anforderung aktivieren
	 * DMA-Anforderung ab einem Sample
	 * kein Fehlerbit im Sample
	 * keine Verschiebung auf 8 Bit
	 */
	adc_fifo_setup(
		true,
		true,
		1,
		false,
		false
	);

	adc_fifo_drain();

	memset(&stream_stats,
	       0,
	       sizeof(stream_stats));

	initialized = true;

	return 0;
}


int adc_dma_start(uint32_t blocks)
{
	if (!initialized) {
		return -ENODEV;
	}

	if (blocks == 0) {
		return -EINVAL;
	}

	if (stream_stats.running) {
		return -EBUSY;
	}

	/*
	 * Eine neue Aufnahme darf erst beginnen, wenn der
	 * Ausgabethread alle alten Blöcke abgearbeitet hat.
	 */
	if (!all_blocks_free()) {
		return -EBUSY;
	}

	memset(&stream_stats,
	       0,
	       sizeof(stream_stats));

	stream_stats.running = true;
	stream_stats.blocks_remaining = blocks;

	ready_read = 0;
	ready_write = 0;

	adc_fifo_drain();

	int first = find_free_block();

	if (first < 0) {
		stream_stats.running = false;
		return -ENOSPC;
	}

	dma_block = (uint8_t)first;

	/*
	 * Ein DMA-Block überträgt 1024 ADC-Samples vom
	 * ADC-FIFO in einen Ringpuffer.
	 */
	struct dma_block_config block = {
		.source_address =
			(uint32_t)(uintptr_t)
				&adc_hw->fifo,

		.dest_address =
			(uint32_t)(uintptr_t)
				sample_ring[dma_block],

		.block_size =
			sizeof(sample_ring[dma_block]),

		.source_addr_adj =
			DMA_ADDR_ADJ_NO_CHANGE,

		.dest_addr_adj =
			DMA_ADDR_ADJ_INCREMENT,
	};

	struct dma_config config = {
		.dma_slot =
			RPI_PICO_DMA_DREQ_TO_SLOT(
				DREQ_ADC
			),

		.channel_direction =
			PERIPHERAL_TO_MEMORY,

		.source_data_size =
			sizeof(uint16_t),

		.dest_data_size =
			sizeof(uint16_t),

		.source_burst_length = 1,
		.dest_burst_length = 1,

		.block_count = 1,
		.head_block = &block,

		.dma_callback = dma_done,
		.user_data = NULL,

		.channel_priority = 1,
	};

	int ret = dma_config(
		dma_dev,
		dma_channel,
		&config
	);

	if (ret != 0) {
		atomic_set(
			&block_states[dma_block],
			BLOCK_FREE
		);

		stream_stats.running = false;

		return ret;
	}

	/*
	 * Kurze Verzögerung, damit der Shell-Befehl vollständig
	 * ausgegeben werden kann.
	 */
	k_work_schedule(
		&start_work,
		K_MSEC(150)
	);

	return 0;
}


void adc_dma_stop(void)
{
	if (!initialized) {
		return;
	}

	stream_stats.running = false;

	/*
	 * Einen möglicherweise noch nicht gestarteten,
	 * verzögerten Start abbrechen.
	 */
	(void)k_work_cancel_delayable(
		&start_work
	);

	adc_run(false);

	/*
	 * Laufenden DMA-Kanal anhalten.
	 */
	if (dma_channel >= 0) {
		(void)dma_stop(
			dma_dev,
			dma_channel
		);
	}

	/*
	 * Kurz warten, bis der DMA-Kanal nicht mehr aktiv ist.
	 */
	struct dma_status status;

	for (int i = 0; i < 20; ++i) {
		int ret = dma_get_status(
			dma_dev,
			dma_channel,
			&status
		);

		if (ret == 0 && !status.busy) {
			break;
		}

		k_sleep(K_MSEC(1));
	}

	/*
	 * Der aktuell vom DMA reservierte Block wird wieder
	 * freigegeben.
	 */
	if (dma_block < RING_BLOCKS) {
		atomic_set(
			&block_states[dma_block],
			BLOCK_FREE
		);
	}

	adc_fifo_drain();
}


void adc_dma_get_stats(
	struct adc_stream_stats *stats)
{
	if (stats == NULL) {
		return;
	}

	*stats = stream_stats;
}
