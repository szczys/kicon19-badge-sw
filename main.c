#include "asf.h"
#include "buttons.h"
#include "io_capture.h"
#include "udc.h"
#include "udi_cdc.h"
#include "commands.h"
#include "lcd.h"
#include "SSD1306_commands.h"


/** Set default LED blink period to 250ms*3 */
#define DEFAULT_LED_FREQ   4

/** LED blink period */
#define LED_BLINK_PERIOD    3

/** LED blink period */
static volatile uint32_t led_blink_period = 0;
volatile bool g_interrupt_enabled = true;


/**
 *  \brief Handler for System Tick interrupt.
 *
 *  Process System Tick Event
 */
void SysTick_Handler(void)
{
}

/**
 * \brief Interrupt handler for TC0 interrupt. Toggles the state of LEDs.
 */
void TC0_Handler(void)
{
    /* Clear status bit to acknowledge interrupt */
    tc_get_status(TC0, 0);

    led_blink_period++;

    if (led_blink_period == LED_BLINK_PERIOD) {
        int buttons = btn_state();

        if(buttons)
            uart_write(UART0, buttons + 0x30);

        pio_toggle_pin(PIO_PA8_IDX);
        led_blink_period = 0;
    }
}

/**
 * \brief Configure Timer Counter 0 to generate an interrupt with the specific
 * frequency.
 *
 * \param freq Timer counter frequency.
 */
static void configure_tc(uint32_t freq)
{
    uint32_t ul_div;
    uint32_t ul_tcclks;
    uint32_t ul_sysclk = sysclk_get_cpu_hz();

    /* Disable TC first */
    tc_stop(TC0, 0);
    tc_disable_interrupt(TC0, 0, TC_IER_CPCS);

    /** Configure TC with the frequency and trigger on RC compare. */
    tc_find_mck_divisor(freq, ul_sysclk, &ul_div, &ul_tcclks, ul_sysclk);
    tc_init(TC0, 0, ul_tcclks | TC_CMR_CPCTRG);
    tc_write_rc(TC0, 0, (ul_sysclk / ul_div) / 4);

    /* Configure and enable interrupt on RC compare */
    NVIC_EnableIRQ((IRQn_Type)ID_TC0);
    tc_enable_interrupt(TC0, 0, TC_IER_CPCS);

    /** Start the counter. */
    tc_start(TC0, 0);
}

static void configure_console(void)
{
    sam_uart_opt_t uart_settings;
    uart_settings.ul_mck = sysclk_get_peripheral_hz();
    uart_settings.ul_baudrate = 115200Ul;
    uart_settings.ul_mode = UART_MR_PAR_NO;

    pio_configure(PIOA, PIO_PERIPH_A, (PIO_PA9A_URXD0 | PIO_PA10A_UTXD0), PIO_DEFAULT);
    sysclk_enable_peripheral_clock(ID_UART0);
    uart_init(UART0, &uart_settings);
}

/**
 * \brief Configure timer ISR to fire regularly.
 */
static void init_timer_isr(void)
{
    SysTick_Config((sysclk_get_cpu_hz() / 1000) * 100); // 100 ms
}

/**
 * \brief initialize pins, watchdog, etc.
 */
static void init_system(void)
{
    /* Disable the watchdog */
    wdt_disable(WDT);

    irq_initialize_vectors();
    cpu_irq_enable();

    /* Initialize the system clock */
    sysclk_init();

    /* Configure LED pins */
    pio_configure(PIOA, PIO_OUTPUT_0, (PIO_PA7 | PIO_PA8), PIO_DEFAULT);

    /* Enable PMC clock for key/slider PIOs  */
    pmc_enable_periph_clk(ID_PIOA);
    pmc_enable_periph_clk(ID_PIOB);

    /* Configure PMC */
    pmc_enable_periph_clk(ID_TC0);

    /* Configure the default TC frequency */
    configure_tc(DEFAULT_LED_FREQ);

    /* Enable the serial console */
    configure_console();

    btn_init();
    SSD1306_init();

    ioc_init();
    ioc_set_clock(F1MHZ);

    /* Start USB stack to authorize VBus monitoring */
    udc_start();
}

uint8_t samples[32];

void ioc_show(void* param) {
    uint8_t* data = (uint8_t*)(param);

    for(int i = 0; i < 32; ++i) {
        uart_write(UART0, 0x30 + data[i]);
        for(int j = 0; j < 65535; ++j) __NOP;
    }
}

////////spi
/* Chip select. */
#define SPI_CHIP_SEL 1
#define SPI_CHIP_PCS spi_get_pcs(SPI_CHIP_SEL)

/* Clock polarity. */
#define SPI_CLK_POLARITY 0

/* Clock phase. */
#define SPI_CLK_PHASE 0

/* Delay before SPCK. */
#define SPI_DLYBS 0x40

/* Delay between consecutive transfers. */
#define SPI_DLYBCT 0x10

/* SPI clock setting (Hz). */
static uint32_t gs_ul_spi_clock = 500000;


void spi_master_transfer(void *p_buf, uint32_t size)
{
	uint32_t i;
	uint8_t uc_pcs;
	static uint16_t data;

	uint8_t *p_buffer;

	p_buffer = p_buf;

	for (i = 0; i < size; i++) {
		spi_write(SPI, p_buffer[i], 0, 0);
		/* Wait transfer done. */
//		while ((spi_read_status(SPI) & SPI_SR_RDRF) == 0);
//		spi_read(SPI, &data, &uc_pcs);
//		p_buffer[i] = data;
	}
}
///////////////////////

///////adc
/** Tracking Time*/
#define TRACKING_TIME			1
/** Transfer Period */
#define TRANSFER_PERIOD			1
#define STARTUP_TIME			3
/** Sample & Hold Time */
#define SAMPLE_HOLD_TIME		6
/** Total number of ADC channels in use */
#define NUM_CHANNELS			2
/** Size of the receive buffer and transmit buffer. */
#define BUFFER_SIZE				NUM_CHANNELS*LCD_WIDTH
/** Reference voltage for ADC, in mv. */
#define VOLT_REF				3300
#define MAX_DIGITAL				4095
#define RESOLUTION(PAGES)		(PAGES*8-1)/MAX_DIGITAL

enum adc_channel_num_t ch_list[NUM_CHANNELS] = {ADC_CHANNEL_3, ADC_CHANNEL_9};
uint16_t us_value[BUFFER_SIZE];
uint16_t adc0_buffer[BUFFER_SIZE/2];
uint32_t adc0_offset_pages;
uint16_t adc1_buffer[BUFFER_SIZE/2];
uint32_t adc_active_channels = 1;
uint32_t adc_buffer_size;
int adc0_buffer_rdy=0;
int adc1_buffer_rdy=0;

/** number of lcd pages for a channel*/
uint32_t adc_pages_per_channel;

/**
 * \brief Read converted data through PDC channel.
 *
 * \param p_adc The pointer of adc peripheral.
 * \param p_s_buffer The destination buffer.
 * \param ul_size The size of the buffer.
 */
static uint32_t adc_read_buffer(Adc * p_adc, uint16_t * p_s_buffer, uint32_t ul_size)
{
	/* Check if the first PDC bank is free. */
	if ((p_adc->ADC_RCR == 0) && (p_adc->ADC_RNCR == 0)) {
		p_adc->ADC_RPR = (uint32_t) p_s_buffer;
		p_adc->ADC_RCR = ul_size;
		p_adc->ADC_PTCR = ADC_PTCR_RXTEN;

		return 1;
	} else { /* Check if the second PDC bank is free. */
		if (p_adc->ADC_RNCR == 0) {
			p_adc->ADC_RNPR = (uint32_t) p_s_buffer;
			p_adc->ADC_RNCR = ul_size;

			return 1;
		} else {
			return 0;
		}
	}
}

/**
 * \brief Initialize ADC, interrupts and PDC transfer.
 *
 * \param adc_ch The pointer of channels names array.
 * \param ul_size Number of channels.
 */
void adc_initialize(enum adc_channel_num_t *adc_ch, uint32_t ul_size)
{
//	if (ul_size > NUM_CHANNELS) return;

	/* Disable PDC channel interrupt. */
	adc_disable_interrupt(ADC, 0xFFFFFFFF);
	adc_disable_all_channel(ADC);

	/* Initialize variables according to number of channels used */
	if(ul_size == 1)
	{
		adc_active_channels = 1;
		adc_buffer_size = BUFFER_SIZE/2;
		adc_pages_per_channel = LCD_PAGES;

	}else
	{
		adc_active_channels = 2;
		adc_buffer_size = BUFFER_SIZE;
		adc_pages_per_channel = LCD_PAGES/2;
		adc0_offset_pages = 4;
	}


	/* Initialize ADC. */
	/*
	 * Formula: ADCClock = MCK / ( (PRESCAL+1) * 2 )
	 * For example, MCK = 64MHZ, PRESCAL = 4, then:
	 * ADCClock = 64 / ((4+1) * 2) = 6.4MHz;
	 */

	/* Formula:
	 *     Startup  Time = startup value / ADCClock
	 *     Startup time = 64 / 100kHz
	 *     here 64/100kHz
	 */
	adc_init(ADC, sysclk_get_cpu_hz(), 100, ADC_STARTUP_TIME_4);

	/* Formula:
	 *     Transfer Time = (TRANSFER * 2 + 3) / ADCClock
	 *     Tracking Time = (TRACKTIM + 1) / ADCClock
	 *     Settling Time = settling value / ADCClock
	 *
	 *     Transfer Time = (1 * 2 + 3) / 100kHz
	 *     Tracking Time = (1 + 1) / 100kHz
	 *     Settling Time = 3 / 100kHz
	 */
	adc_configure_timing(ADC, TRACKING_TIME, ADC_SETTLING_TIME_3, TRANSFER_PERIOD);

	/* Enable channel number tag. */
	adc_enable_tag(ADC);

	NVIC_EnableIRQ(ADC_IRQn);

	/* Enable channels. */
	for(int i = 0; i < ul_size; i++)
	{
		adc_enable_channel(ADC, adc_ch[i]);
	}

	adc_configure_trigger(ADC, ADC_TRIG_SW, 1);

	adc_start(ADC);

	/* Enable PDC channel interrupt. */
	adc_enable_interrupt(ADC, ADC_IER_RXBUFF);
	/* Start new pdc transfer. */
	adc_read_buffer(ADC, us_value, adc_buffer_size);
}
/////////

int main(void)
{
    const uint8_t *cmd_resp;
    uint8_t i, j = 0;

    /* Initialize the SAM system */
    init_system();

    /* Configure timer ISR to fire regularly */
    init_timer_isr();

    uart_write(UART0, 'U');

    // TODO remove
    pio_configure(PIOA, PIO_OUTPUT_0, (PIO_PA20 | PIO_PA22), PIO_DEFAULT);
    ioc_set_handler(ioc_show, samples);
    ioc_fetch(samples, 32);

//
//    uint8_t siema[5]= "siema";
//    SSD1306_setString(0,0,siema, 5,1);
//    SSD1306_drawBitmap();
////
//	siema[5]= "abcde";

//    if(!SSD1306_isBusy())
//    {
//		SSD1306_setString(0,0,siema, 5,1);
//		SSD1306_setLine(15, 15, 20, 60,0);
//		SSD1306_drawBitmapDMA();
//    }

//////////////////// spi init
    pio_configure(PIOA, PIO_PERIPH_A,
            (PIO_PA12A_MISO | PIO_PA13A_MOSI | PIO_PA14A_SPCK ), PIO_OPENDRAIN | PIO_PULLUP);

    pio_configure(PIOB, PIO_PERIPH_A,
              (PIO_PB14A_NPCS1), PIO_OPENDRAIN | PIO_PULLUP);

	spi_enable_clock(SPI);
	spi_disable(SPI);
	spi_reset(SPI);
	spi_set_master_mode(SPI);
	spi_disable_mode_fault_detect(SPI);
	spi_disable_loopback(SPI);
	spi_set_peripheral_chip_select_value(SPI, SPI_CHIP_PCS);
	spi_set_clock_polarity(SPI, SPI_CHIP_SEL, SPI_CLK_POLARITY);
	spi_set_clock_phase(SPI, SPI_CHIP_SEL, SPI_CLK_PHASE);
	spi_set_bits_per_transfer(SPI, SPI_CHIP_SEL,SPI_CSR_BITS_8_BIT);
	spi_set_baudrate_div(SPI, SPI_CHIP_SEL,	(sysclk_get_peripheral_hz()/ gs_ul_spi_clock));
	spi_set_transfer_delay(SPI, SPI_CHIP_SEL, SPI_DLYBS,SPI_DLYBCT);
	spi_enable(SPI);

	uint8_t spi_buffer[1]={0x0A};


///////////////////////////////////////////////////adc
    pmc_enable_periph_clk(ID_ADC);
    pio_configure(PIOA, PIO_INPUT, PIO_PA20X1_AD3, PIO_DEFAULT);
    pio_configure(PIOA, PIO_INPUT, PIO_PA22X1_AD9, PIO_DEFAULT);

    adc_initialize(ch_list, 2);

int updated=0;

    /* Loop forever */
    for (;;) {
        if (udi_cdc_is_rx_ready()) {
            cmd_new_data(udi_cdc_getc());
            cmd_resp = cmd_try_execute();

            if (cmd_resp) {
                for (i = 0; i < cmd_raw_len(cmd_resp); ++i) {
                    udi_cdc_putc(cmd_resp[i]);
                    uart_write(UART0, cmd_resp[i]); // TODO remove
                }
            }
        }

        pio_toggle_pin(PIO_PA7_IDX);

        if(j % 10 == 0)
            pio_toggle_pin(PIO_PA20_IDX);

        if(j % 30 == 0)
            pio_toggle_pin(PIO_PA22_IDX);

        ++j;

        for(uint16_t i = 0; i < 65535; ++i) __NOP;

        spi_master_transfer(spi_buffer,1);

        if(adc0_buffer_rdy) // into a function? make it any smarter with offsets?
        {
        	if(!SSD1306_isBusy())
			{
        		SSD1306_clearBuffer(0, adc0_offset_pages, WHITE, adc_pages_per_channel*LCD_WIDTH );
        		for(int i = 0; i<(adc_buffer_size); i++)
        		{
        			SSD1306_setPixel(i, adc0_buffer[i], 0);
				}
//        		SSD1306_drawBitmapDMA();
        		adc0_buffer_rdy=0;
        		updated++;
        	}

        }

        if(adc1_buffer_rdy)
        {
        	if(!SSD1306_isBusy())
			{
        		SSD1306_clearBuffer(0, 0, WHITE, adc_pages_per_channel*LCD_WIDTH );
        		for(int i = 0; i<(adc_buffer_size); i++)
        		{
        			SSD1306_setPixel(i, adc1_buffer[i], 0);
				}
//        		SSD1306_drawBitmapDMA();
        		adc1_buffer_rdy=0;
        		updated++;
        	}

        }
        if(updated)
        {
        	SSD1306_drawBitmapDMA(); //make any logic whether to draw anything?
        	updated=0;
        }
    }
}



/**
 * \brief Interrupt handler for the ADC.
 */
void ADC_Handler(void)
{
	uint32_t i;
	uint32_t ul_temp;
	uint8_t uc_ch_num;

	uint32_t adcV;
	char buffer[6];

	if ((adc_get_status(ADC) & ADC_ISR_RXBUFF) == ADC_ISR_RXBUFF)
	{
		for (i = 0; i < adc_buffer_size; i++)
		{
			uc_ch_num = (us_value[i] & ADC_LCDR_CHNB_Msk) >> ADC_LCDR_CHNB_Pos;
			if (ch_list[0] == uc_ch_num && !adc0_buffer_rdy)
			{
				adc0_buffer[i] = (us_value[i] & ADC_LCDR_LDATA_Msk)*RESOLUTION(adc_pages_per_channel) + adc0_offset_pages*8;
//				adc0_buffer_rdy = 1;
			}
			else if (ch_list[1] == uc_ch_num && !adc1_buffer_rdy)
			{
				adc1_buffer[i] = (us_value[i] & ADC_LCDR_LDATA_Msk)*RESOLUTION(adc_pages_per_channel);
//				adc1_buffer_rdy = 1;

//				sprintf(buffer, "%u\r\n", us_value[i]);
//				for(uint16_t i=0; i<6; i++)
//				{
//					while(!uart_is_tx_buf_empty(UART0));
//					uart_write(UART0, buffer[i]);
//				}
			}
		}
		adc0_buffer_rdy = 1; //check which channels active?
		adc1_buffer_rdy = 1;

		/* Start new pdc transfer. */
		adc_read_buffer(ADC, us_value, adc_buffer_size);
	}
}
