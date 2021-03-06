#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>

#include <usbd_core.h>
#include <usbd_cdc.h>

#include "usbd_cdc_if.h"
#include "usbd_desc.h"

USBD_HandleTypeDef USBD_Device;
void SysTick_Handler(void);
void OTG_FS_IRQHandler(void);
void OTG_HS_IRQHandler(void);
extern PCD_HandleTypeDef hpcd;
	
int VCP_read(void *pBuffer, int size);
int VCP_write(const void *pBuffer, int size);
extern char g_VCPInitialized;

#define GPIO_SET_BIT(PORT, BIT)		PORT->BSRR = BIT
#define GPIO_CLR_BIT(PORT, BIT)		PORT->BSRR = (BIT << 16)
#define GPIO_TOGGLE_BIT(PORT, BIT)	PORT->ODR ^= BIT

TIM_HandleTypeDef	TIM2_Handle;
ADC_HandleTypeDef	AdcHandle;
DMA_HandleTypeDef	DmaHandle;

#define		MAX_PACKETS_PER_CHANNEL	1000
#define		MAX_NUM_OF_CHANNELS		(1 + 3 * 3)	// 1 - optional current measurment, 3 * 3 = for 3 accelerometers

uint8_t numOfChannels = MAX_NUM_OF_CHANNELS;
int		packetsPerChannel = MAX_PACKETS_PER_CHANNEL;
uint8_t	dataBuffer[2 * MAX_PACKETS_PER_CHANNEL * MAX_NUM_OF_CHANNELS] = { 0 }; 	// * 2 because of DMA half cplt transfer


enum { 
	IDLE = 0, 
	HALF_CPLT,
	CPLT 
} adcState = IDLE;

// IRQ
/////////////////////////////////////
void SysTick_Handler(void) {
	HAL_IncTick();
	HAL_SYSTICK_IRQHandler();
}

void OTG_FS_IRQHandler(void)
{
	HAL_PCD_IRQHandler(&hpcd);
}
	 
void DMA2_Stream0_IRQHandler() {
	HAL_DMA_IRQHandler(&DmaHandle);
}

#ifdef DEBUG
void TIM2_IRQHandler() {	
	GPIO_TOGGLE_BIT(GPIOA, GPIO_PIN_8);
	TIM2->SR &= ~(TIM_SR_UIF);
}
#endif //  DEBUG

/////////////////////////////////////		

// IRQ Callbacks
/////////////////////////////////////
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef* hadc) {
	adcState = CPLT;
	
#ifdef DEBUG
	GPIO_TOGGLE_BIT(GPIOB, GPIO_PIN_9);
#endif //  DEBUG
}
	
void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef* hadc) {
	adcState = HALF_CPLT;
}
/////////////////////////////////////

static void SystemClock_Config(void) {
	RCC_ClkInitTypeDef RCC_ClkInitStruct;
	RCC_OscInitTypeDef RCC_OscInitStruct;

	__PWR_CLK_ENABLE();
	__HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

	RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
	RCC_OscInitStruct.HSEState = RCC_HSE_ON;
	RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
	RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
	RCC_OscInitStruct.PLL.PLLM = 8;
	RCC_OscInitStruct.PLL.PLLN = 336;
	RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
	RCC_OscInitStruct.PLL.PLLQ = 7;
	HAL_RCC_OscConfig(&RCC_OscInitStruct);

	RCC_ClkInitStruct.ClockType = (RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2);

	RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
	RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
	RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
	RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;
	HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5);
}

void DMA_Configure() {
	__HAL_RCC_DMA2_CLK_ENABLE();
	DmaHandle.Instance = DMA2_Stream0;
  
	DmaHandle.Init.Channel  = DMA_CHANNEL_0;
	DmaHandle.Init.Direction = DMA_PERIPH_TO_MEMORY;
	DmaHandle.Init.PeriphInc = DMA_PINC_DISABLE;
	DmaHandle.Init.MemInc = DMA_MINC_ENABLE;
	DmaHandle.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
	DmaHandle.Init.MemDataAlignment = DMA_PDATAALIGN_BYTE;
	DmaHandle.Init.Mode = DMA_CIRCULAR;
	DmaHandle.Init.Priority = DMA_PRIORITY_HIGH;
	DmaHandle.Init.FIFOMode = DMA_FIFOMODE_DISABLE;         
	DmaHandle.Init.FIFOThreshold = DMA_FIFO_THRESHOLD_HALFFULL;
	DmaHandle.Init.MemBurst = DMA_MBURST_SINGLE;
	DmaHandle.Init.PeriphBurst = DMA_PBURST_SINGLE; 
    
	HAL_DMA_Init(&DmaHandle);
    
	__HAL_LINKDMA(&AdcHandle, DMA_Handle, DmaHandle);
 
	HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, 0, 0);   
	HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);
}


void ADC_Configure() {
	GPIO_InitTypeDef	GPIO_InitStructure;

	// ADC Channel / GPIO Pin
	//  ADC1_CH-0  / GPIOA_0
	//  ADC1_CH-1  / GPIOA_1
	//  ADC1_CH-2  / GPIOA_2
	
	//  ADC1_CH-3  / GPIOA_3
	//  ADC1_CH-8  / GPIOB_0
	//  ADC1_CH-9  / GPIOB_1	
	
	//  ADC1_CH-11 / GPIOC_1
	//  ADC1_CH-12 / GPIOC_2
	//  ADC1_CH-14 / GPIOC_4	
	
	//  ADC1_CH-15 / GPIOC_5
		
	__HAL_RCC_ADC1_CLK_ENABLE();
	
	// PORT A
	__HAL_RCC_GPIOA_CLK_ENABLE();
	GPIO_InitStructure.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3;
	GPIO_InitStructure.Mode = GPIO_MODE_ANALOG;
	GPIO_InitStructure.Pull = GPIO_NOPULL;
	HAL_GPIO_Init(GPIOA, &GPIO_InitStructure);
	
	// PORT B
	__HAL_RCC_GPIOB_CLK_ENABLE();
	GPIO_InitStructure.Pin = GPIO_PIN_0 | GPIO_PIN_1;
	HAL_GPIO_Init(GPIOB, &GPIO_InitStructure);
	
	// PORT B
	__HAL_RCC_GPIOC_CLK_ENABLE();
	GPIO_InitStructure.Pin = GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_4 | GPIO_PIN_5;
	HAL_GPIO_Init(GPIOC, &GPIO_InitStructure);
	
	AdcHandle.Instance = ADC1;
	AdcHandle.Init.ClockPrescaler = ADC_CLOCKPRESCALER_PCLK_DIV4;
	AdcHandle.Init.Resolution = ADC_RESOLUTION_8B;
	AdcHandle.Init.ScanConvMode = ENABLE;
	AdcHandle.Init.ContinuousConvMode = DISABLE;
	AdcHandle.Init.DiscontinuousConvMode = DISABLE;
	AdcHandle.Init.NbrOfDiscConversion = 0;
	AdcHandle.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_RISING;
	AdcHandle.Init.ExternalTrigConv = ADC_EXTERNALTRIGCONV_T2_TRGO;
	AdcHandle.Init.DataAlign = ADC_DATAALIGN_RIGHT;
	AdcHandle.Init.NbrOfConversion = numOfChannels;
	AdcHandle.Init.DMAContinuousRequests = ENABLE;
	AdcHandle.Init.EOCSelection = ADC_EOC_SEQ_CONV;
	HAL_ADC_Init(&AdcHandle);
	
	ADC_ChannelConfTypeDef adcChannelConfig;

	// Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time. 
	adcChannelConfig.Channel = ADC_CHANNEL_0;
	adcChannelConfig.Rank = 1;
	adcChannelConfig.SamplingTime = ADC_SAMPLETIME_28CYCLES;  	// ADC_SAMPLETIME_84CYCLES
	if (HAL_ADC_ConfigChannel(&AdcHandle, &adcChannelConfig) != HAL_OK) {}

	adcChannelConfig.Channel = ADC_CHANNEL_1;
	adcChannelConfig.Rank = 2;
	if (HAL_ADC_ConfigChannel(&AdcHandle, &adcChannelConfig) != HAL_OK) {}
	
	adcChannelConfig.Channel = ADC_CHANNEL_2;
	adcChannelConfig.Rank = 3;
	if (HAL_ADC_ConfigChannel(&AdcHandle, &adcChannelConfig) != HAL_OK) {}
	
	adcChannelConfig.Channel = ADC_CHANNEL_3;
	adcChannelConfig.Rank = 4;
	if (HAL_ADC_ConfigChannel(&AdcHandle, &adcChannelConfig) != HAL_OK) {}
	
	adcChannelConfig.Channel = ADC_CHANNEL_8;
	adcChannelConfig.Rank = 5;
	if (HAL_ADC_ConfigChannel(&AdcHandle, &adcChannelConfig) != HAL_OK) {}
	
	adcChannelConfig.Channel = ADC_CHANNEL_9;
	adcChannelConfig.Rank = 6;
	if (HAL_ADC_ConfigChannel(&AdcHandle, &adcChannelConfig) != HAL_OK) {}
	
	adcChannelConfig.Channel = ADC_CHANNEL_11;
	adcChannelConfig.Rank = 7;
	if (HAL_ADC_ConfigChannel(&AdcHandle, &adcChannelConfig) != HAL_OK) {}
	
	adcChannelConfig.Channel = ADC_CHANNEL_12;
	adcChannelConfig.Rank = 8;
	if (HAL_ADC_ConfigChannel(&AdcHandle, &adcChannelConfig) != HAL_OK) {}
	
	adcChannelConfig.Channel = ADC_CHANNEL_14;
	adcChannelConfig.Rank = 9;
	if (HAL_ADC_ConfigChannel(&AdcHandle, &adcChannelConfig) != HAL_OK) {}
	
	adcChannelConfig.Channel = ADC_CHANNEL_15;
	adcChannelConfig.Rank = 10;
	if (HAL_ADC_ConfigChannel(&AdcHandle, &adcChannelConfig) != HAL_OK) {}
}

void TIM_Configure() {
	__TIM2_CLK_ENABLE();
	
	TIM2->PSC = 83;				// Set the Prescaler value: 20, that comes to one tick being 249 ns
	TIM2->ARR = 1000;  			// Reload timer
	TIM2->EGR = TIM_EGR_UG;  	// Reset the counter and generate update event		
	TIM2->CR2 |= TIM_CR2_MMS_1;		
	
#ifdef DEBUG
	TIM2->DIER |= TIM_DIER_UIE;
	HAL_NVIC_SetPriority(TIM2_IRQn, 1, 0);
	HAL_NVIC_EnableIRQ(TIM2_IRQn);			  
#endif //  DEBUG

	TIM2->CR1 |= TIM_CR1_CEN;
}

#ifdef DEBUG
void GPIO_Configure() {
	GPIO_InitTypeDef	GPIO_InitStructure;
	
	__HAL_RCC_GPIOA_CLK_ENABLE();
	GPIO_InitStructure.Pin = GPIO_PIN_8;
	GPIO_InitStructure.Mode = GPIO_MODE_OUTPUT_PP;
	GPIO_InitStructure.Pull = GPIO_NOPULL;
	HAL_GPIO_Init(GPIOA, &GPIO_InitStructure);
	
	__HAL_RCC_GPIOB_CLK_ENABLE();
	GPIO_InitStructure.Pin = GPIO_PIN_9;
	HAL_GPIO_Init(GPIOB, &GPIO_InitStructure);
}
#endif //  DEBUG

static void Init() {
	HAL_Init();	
	SystemClock_Config();

	USBD_Init(&USBD_Device, &VCP_Desc, 0);

	USBD_RegisterClass(&USBD_Device, &USBD_CDC);
	USBD_CDC_RegisterInterface(&USBD_Device, &USBD_CDC_MyUSB_fops);
	USBD_Start(&USBD_Device);
	
	// Wait for USB to Initialize
	while (USBD_Device.pClassData == 0) {
	}			
	
#ifdef DEBUG
	GPIO_Configure();
#endif //  DEBUG
	ADC_Configure();
	DMA_Configure();
	TIM_Configure();	
}

bool ParameterInit() {
	uint8_t rxBuf[20] = { 0 };
	int read = 0;
	uint32_t val = 0;
	
	// Wait for go
	while (1) {
		memset(rxBuf, 0, sizeof(rxBuf));
		if ((read = VCP_read(rxBuf, sizeof(rxBuf))) > 0) {
			if (rxBuf[0] == 'g' && rxBuf[1] == 'o') {
				break;
			}
		}
	}
	
	// Number of channels
	memset(rxBuf, 0, sizeof(rxBuf));		
	while((read = VCP_read(rxBuf, sizeof(rxBuf))) <= 0);	
	val = atoi((const char*)rxBuf);
	if (0 < val && val < 11) numOfChannels = val;
	else return false;
	
	// Number of packets per channel
	memset(rxBuf, 0, sizeof(rxBuf));		
	while ((read = VCP_read(rxBuf, sizeof(rxBuf))) <= 0);	
	val = atoi((const char*)rxBuf);
	if (0 < val && val <= MAX_PACKETS_PER_CHANNEL) packetsPerChannel = val;
	else return false;
	
	// Sample frequency
	memset(rxBuf, 0, sizeof(rxBuf));	
	while((read = VCP_read(rxBuf, sizeof(rxBuf))) <= 0);
	val = atoi((const char*)rxBuf);
	if (val > 0 && val <= 1e6) {		
		TIM2->ARR = (int)((float)1e6 / (float)val);   // val is always positive so this round hack is ok
		TIM2->EGR = TIM_EGR_UG; 
	} else return false;
	
	AdcHandle.Init.NbrOfConversion = numOfChannels;	
	HAL_ADC_Init(&AdcHandle);
	
	return true;
}

int main() {
	Init();
	while (!ParameterInit());
	
	int DataTransferSize = packetsPerChannel * numOfChannels * sizeof(uint8_t);
	
	HAL_ADC_Start_DMA(&AdcHandle, (uint32_t*)dataBuffer, DataTransferSize * 2 /* "*2" because we are using DMA half complete callback, sort of "double buffering" */);
	
	uint8_t* FirstHalf = dataBuffer;
	uint8_t* SecondHalf = &dataBuffer[DataTransferSize];
	
	while (1) {
		if (adcState == HALF_CPLT) {
			adcState = IDLE;
			VCP_write(FirstHalf, DataTransferSize);
		} else if (adcState == CPLT) {
			adcState = IDLE;
			VCP_write(SecondHalf, DataTransferSize);
		}			
	}
}
