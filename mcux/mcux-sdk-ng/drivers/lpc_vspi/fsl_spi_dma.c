/*
 * Copyright 2017-2019 NXP
 * All rights reserved.
 *
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "fsl_spi_dma.h"

/*******************************************************************************
 * Definitions
 ******************************************************************************/

/* Component ID definition, used by tools. */
#ifndef FSL_COMPONENT_ID
#define FSL_COMPONENT_ID "platform.drivers.vspi_dma"
#endif

/*<! Structure definition for spi_dma_private_handle_t. The structure is private. */
typedef struct _spi_dma_private_handle
{
    SPI_Type *base;
    spi_dma_handle_t *handle;
} spi_dma_private_handle_t;

/*! @brief SPI transfer state, which is used for SPI transactiaonl APIs' internal state. */
enum _spi_dma_states_t
{
    kSPI_Idle = 0x0, /*!< SPI is idle state */
    kSPI_Busy        /*!< SPI is busy tranferring data. */
};

typedef struct _spi_dma_txdummy
{
    uint32_t lastWord;
    uint32_t word;
} spi_dma_txdummy_t;

/*<! Private handle only used for internally. */
static spi_dma_private_handle_t s_dmaPrivateHandle[FSL_FEATURE_SOC_SPI_COUNT];
/*******************************************************************************
 * Prototypes
 ******************************************************************************/

/*!
 * @brief DMA callback function for SPI send transfer.
 *
 * @param handle DMA handle pointer.
 * @param userData User data for DMA callback function.
 */
static void SPI_TxDMACallback(dma_handle_t *handle, void *userData, bool transferDone, uint32_t intmode);

/*!
 * @brief DMA callback function for SPI receive transfer.
 *
 * @param handle DMA handle pointer.
 * @param userData User data for DMA callback function.
 */
static void SPI_RxDMACallback(dma_handle_t *handle, void *userData, bool transferDone, uint32_t intmode);

/*******************************************************************************
 * Variables
 ******************************************************************************/
#if defined(__ICCARM__)
#pragma data_alignment                                        = 4
static spi_dma_txdummy_t s_txDummy[FSL_FEATURE_SOC_SPI_COUNT] = {0};
#elif defined(__CC_ARM) || defined(__ARMCC_VERSION)
__attribute__((aligned(4))) static spi_dma_txdummy_t s_txDummy[FSL_FEATURE_SOC_SPI_COUNT] = {0};
#elif defined(__GNUC__)
__attribute__((aligned(4))) static spi_dma_txdummy_t s_txDummy[FSL_FEATURE_SOC_SPI_COUNT] = {0};
#endif

#if defined(__ICCARM__)
#pragma data_alignment = 4
static uint16_t s_rxDummy;
static uint32_t s_txLastData[FSL_FEATURE_SOC_SPI_COUNT];
#elif defined(__CC_ARM) || defined(__ARMCC_VERSION)
__attribute__((aligned(4))) static uint16_t s_rxDummy;
__attribute__((aligned(4))) static uint32_t s_txLastData[FSL_FEATURE_SOC_SPI_COUNT];
#elif defined(__GNUC__)
__attribute__((aligned(4))) static uint16_t s_rxDummy;
__attribute__((aligned(4))) static uint32_t s_txLastData[FSL_FEATURE_SOC_SPI_COUNT];
#endif

#if defined(__ICCARM__)
#pragma data_alignment                                                    = 16
static dma_descriptor_t s_spi_descriptor_table[FSL_FEATURE_SOC_SPI_COUNT] = {0};
#elif defined(__CC_ARM) || defined(__ARMCC_VERSION)
__attribute__((aligned(16))) static dma_descriptor_t s_spi_descriptor_table[FSL_FEATURE_SOC_SPI_COUNT] = {0};
#elif defined(__GNUC__)
__attribute__((aligned(16))) static dma_descriptor_t s_spi_descriptor_table[FSL_FEATURE_SOC_SPI_COUNT] = {0};
#endif

/*******************************************************************************
 * Code
 ******************************************************************************/

static void XferToFifoWR(spi_transfer_t *xfer, uint32_t *fifowr)
{
    *fifowr |= ((xfer->configFlags & (uint32_t)kSPI_FrameDelay) != 0U) ? (uint32_t)kSPI_FrameDelay : 0U;
    *fifowr |= ((xfer->configFlags & (uint32_t)kSPI_FrameAssert) != 0U) ? (uint32_t)kSPI_FrameAssert : 0U;
    *fifowr |= ((xfer->configFlags & (uint32_t)kSPI_ReceiveIgnore) != 0U) ? (uint32_t)kSPI_ReceiveIgnore : 0U;
}

static void SpiConfigToFifoWR(spi_config_t *config, uint32_t *fifowr)
{
    *fifowr |= ((uint32_t)SPI_DEASSERT_ALL & (uint32_t)(~SPI_DEASSERT_SSELNUM((uint32_t)config->sselNum)));
    /* set width of data - range asserted at entry */
    *fifowr |= SPI_TXDATCTL_LEN(config->dataWidth);
}

static void SPI_SetupDummy(SPI_Type *base, spi_dma_txdummy_t *dummy, spi_transfer_t *xfer, spi_config_t *spi_config_p)
{
    uint32_t instance  = SPI_GetInstance(base);
    uint32_t dummydata = (uint32_t)s_dummyData[instance];
    dummydata |= ((uint32_t)s_dummyData[instance] << 8U);

    dummy->word = dummydata;
    XferToFifoWR(xfer, &dummy->word);
    SpiConfigToFifoWR(spi_config_p, &dummy->word);
    if (((xfer->configFlags & (uint32_t)kSPI_FrameAssert) != 0U) &&
        (((uint8_t)spi_config_p->dataWidth > 7U) ? (xfer->dataSize > 2U) : (xfer->dataSize > 1U)))
    {
        dummy->lastWord = dummydata;
        XferToFifoWR(xfer, &dummy->lastWord);
        SpiConfigToFifoWR(spi_config_p, &dummy->lastWord);
        dummy->word &= (~(uint32_t)kSPI_FrameAssert);
    }
}

/*!
 * brief Initialize the SPI master DMA handle.
 *
 * This function initializes the SPI master DMA handle which can be used for other SPI master transactional APIs.
 * Usually, for a specified SPI instance, user need only call this API once to get the initialized handle.
 *
 * param base SPI peripheral base address.
 * param handle SPI handle pointer.
 * param callback User callback function called at the end of a transfer.
 * param userData User data for callback.
 * param txHandle DMA handle pointer for SPI Tx, the handle shall be static allocated by users.
 * param rxHandle DMA handle pointer for SPI Rx, the handle shall be static allocated by users.
 */
status_t SPI_MasterTransferCreateHandleDMA(SPI_Type *base,
                                           spi_dma_handle_t *handle,
                                           spi_dma_callback_t callback,
                                           void *userData,
                                           dma_handle_t *txHandle,
                                           dma_handle_t *rxHandle)
{
    uint32_t instance = 0;

    /* check 'base' */
    assert(!(NULL == base));
    if (NULL == base)
    {
        return kStatus_InvalidArgument;
    }
    /* check 'handle' */
    assert(!(NULL == handle));
    if (NULL == handle)
    {
        return kStatus_InvalidArgument;
    }

    instance = SPI_GetInstance(base);

    (void)memset(handle, 0, sizeof(*handle));
    /* Set spi base to handle */
    handle->txHandle = txHandle;
    handle->rxHandle = rxHandle;
    handle->callback = callback;
    handle->userData = userData;

    /* Set SPI state to idle */
    handle->state = (uint32_t)kSPI_Idle;

    /* Set handle to global state */
    s_dmaPrivateHandle[instance].base   = base;
    s_dmaPrivateHandle[instance].handle = handle;

    /* Install callback for Tx dma channel */
    DMA_SetCallback(handle->txHandle, SPI_TxDMACallback, &s_dmaPrivateHandle[instance]);
    DMA_SetCallback(handle->rxHandle, SPI_RxDMACallback, &s_dmaPrivateHandle[instance]);

    return kStatus_Success;
}

/*!
 * brief Perform a non-blocking SPI transfer using DMA.
 *
 * note This interface returned immediately after transfer initiates, users should call
 * SPI_GetTransferStatus to poll the transfer status to check whether SPI transfer finished.
 *
 * param base SPI peripheral base address.
 * param handle SPI DMA handle pointer.
 * param xfer Pointer to dma transfer structure.
 * retval kStatus_Success Successfully start a transfer.
 * retval kStatus_InvalidArgument Input argument is invalid.
 * retval kStatus_SPI_Busy SPI is not idle, is running another transfer.
 */
status_t SPI_MasterTransferDMA(SPI_Type *base, spi_dma_handle_t *handle, spi_transfer_t *xfer)
{
    assert(!((NULL == handle) || (NULL == xfer)));

    uint32_t instance;
    status_t result = kStatus_Success;
    spi_config_t *spi_config_p;
    uint32_t address;

    if ((NULL == handle) || (NULL == xfer))
    {
        return kStatus_InvalidArgument;
    }
    /* byte size is zero. */
    assert(!(xfer->dataSize == 0U));
    if (xfer->dataSize == 0U)
    {
        return kStatus_InvalidArgument;
    }

    /* cannot get instance from base address */
    instance = SPI_GetInstance(base);

    /* Check if the device is busy */
    if (handle->state == (uint32_t)kSPI_Busy)
    {
        return kStatus_SPI_Busy;
    }
    else
    {
        uint32_t tmp;
        dma_transfer_config_t xferConfig = {0};
        spi_config_p                     = (spi_config_t *)SPI_GetConfig(base);

        handle->state        = (uint32_t)kStatus_SPI_Busy;
        handle->transferSize = xfer->dataSize;

        /* Receive */
        if (SPI_IsRxFifoEnabled(base))
        {
            address = (uint32_t) & (VFIFO->SPI[instance].RXDATSPI);
        }
        else
        {
            address = (uint32_t)&base->RXDAT;
        }
        if (xfer->rxData != NULL)
        {
            DMA_PrepareTransfer(&xferConfig, (uint32_t *)address, xfer->rxData,
                                (((uint32_t)spi_config_p->dataWidth > 7U) ? (sizeof(uint16_t)) : (sizeof(uint8_t))),
                                xfer->dataSize, kDMA_PeripheralToMemory, NULL);
        }
        else
        {
            DMA_PrepareTransfer(&xferConfig, (uint32_t *)address, &s_rxDummy,
                                (((uint32_t)spi_config_p->dataWidth > 7U) ? (sizeof(uint16_t)) : (sizeof(uint8_t))),
                                xfer->dataSize, kDMA_StaticToStatic, NULL);
        }
        (void)DMA_SubmitTransfer(handle->rxHandle, &xferConfig);
        handle->rxInProgress = true;
        DMA_StartTransfer(handle->rxHandle);

        /* Transmit */
        tmp = 0U;
        XferToFifoWR(xfer, &tmp);
        SpiConfigToFifoWR(spi_config_p, &tmp);

        if (((xfer->configFlags & (uint32_t)kSPI_FrameAssert) != 0U) &&
            (((uint32_t)spi_config_p->dataWidth > 7U) ? (xfer->dataSize > 2U) : (xfer->dataSize > 1U)))
        {
            if ((uint32_t)spi_config_p->dataWidth > 7U)
            {
                s_txLastData[instance] =
                    tmp | ((uint32_t)(xfer->txData[xfer->dataSize - 1U]) << 8U) | (xfer->txData[xfer->dataSize - 2U]);
            }
            else
            {
                s_txLastData[instance] = tmp | (xfer->txData[xfer->dataSize - 1U]);
            }

            /* If not the last data, clear the end of transfer control bit. */
            tmp &= ~((uint32_t)(kSPI_FrameAssert));
        }

        if (xfer->txData != NULL)
        {
            if (SPI_IsTxFifoEnabled(base))
            {
                address = (uint32_t) & (VFIFO->SPI[instance].TXDATSPI);
                if (((xfer->configFlags & (uint32_t)kSPI_FrameAssert) != 0U) &&
                    (((uint32_t)spi_config_p->dataWidth > 7U) ? (xfer->dataSize > 2U) : (xfer->dataSize > 1U)))
                {
                    dma_xfercfg_t tmp_xfercfg = {0};
                    tmp_xfercfg.valid         = true;
                    tmp_xfercfg.swtrig        = true;
                    tmp_xfercfg.intA          = true;
                    tmp_xfercfg.byteWidth     = sizeof(uint32_t);
                    tmp_xfercfg.srcInc        = 0;
                    tmp_xfercfg.dstInc        = 0;
                    tmp_xfercfg.transferCount = 1;
                    /* create chained descriptor to transmit last word */
                    DMA_CreateDescriptor(&s_spi_descriptor_table[instance], &tmp_xfercfg, &s_txLastData[instance],
                                         (uint32_t *)address, NULL);
                    if ((uint32_t)spi_config_p->dataWidth > 7U)
                    {
                        DMA_PrepareTransfer(&xferConfig, xfer->txData, (uint32_t *)address, sizeof(uint16_t),
                                            (xfer->dataSize - 2U), kDMA_MemoryToPeripheral,
                                            &s_spi_descriptor_table[instance]);
                    }
                    else
                    {
                        DMA_PrepareTransfer(&xferConfig, xfer->txData, (uint32_t *)address, sizeof(uint8_t),
                                            (xfer->dataSize - 1U), kDMA_MemoryToPeripheral,
                                            &s_spi_descriptor_table[instance]);
                    }

                    /* Disable interrupts for first descriptor to avoid calling callback twice */
                    xferConfig.xfercfg.intA = false;
                    xferConfig.xfercfg.intB = false;
                }
                else
                {
                    if ((uint32_t)spi_config_p->dataWidth > 7U)
                    {
                        DMA_PrepareTransfer(&xferConfig, xfer->txData, (uint32_t *)address, sizeof(uint16_t),
                                            (xfer->dataSize), kDMA_MemoryToPeripheral, NULL);
                    }
                    else
                    {
                        DMA_PrepareTransfer(&xferConfig, xfer->txData, (uint32_t *)address, sizeof(uint8_t),
                                            (xfer->dataSize), kDMA_MemoryToPeripheral, NULL);
                    }
                }
            }
            else
            {
                address = (uint32_t)&base->TXDATCTL;
                if (((xfer->configFlags & (uint32_t)kSPI_FrameAssert) != 0U) &&
                    (((uint32_t)spi_config_p->dataWidth > 7U) ? (xfer->dataSize > 2U) : (xfer->dataSize > 1U)))
                {
                    dma_xfercfg_t tmp_xfercfg = {0};
                    tmp_xfercfg.valid         = true;
                    tmp_xfercfg.swtrig        = true;
                    tmp_xfercfg.intA          = true;
                    tmp_xfercfg.byteWidth     = sizeof(uint32_t);
                    tmp_xfercfg.srcInc        = 0;
                    tmp_xfercfg.dstInc        = 0;
                    tmp_xfercfg.transferCount = 1;
                    /* Create chained descriptor to transmit last word */
                    DMA_CreateDescriptor(&s_spi_descriptor_table[instance], &tmp_xfercfg, &s_txLastData[instance],
                                         (uint32_t *)address, NULL);
                    if ((uint32_t)spi_config_p->dataWidth > 7U)
                    {
                        DMA_PrepareTransfer(&xferConfig, xfer->txData, (uint32_t *)address, sizeof(uint16_t),
                                            (xfer->dataSize - 2U), kDMA_MemoryToPeripheral,
                                            &s_spi_descriptor_table[instance]);
                    }
                    else
                    {
                        DMA_PrepareTransfer(&xferConfig, xfer->txData, (uint32_t *)address, sizeof(uint8_t),
                                            (xfer->dataSize - 1U), kDMA_MemoryToPeripheral,
                                            &s_spi_descriptor_table[instance]);
                    }
                    /* disable interrupts for first descriptor to avoid calling callback twice */
                    xferConfig.xfercfg.intA = false;
                    xferConfig.xfercfg.intB = false;
                }
                else
                {
                    address = (uint32_t)&base->TXDAT;
                    if ((uint32_t)spi_config_p->dataWidth > 7U)
                    {
                        DMA_PrepareTransfer(&xferConfig, xfer->txData, (uint32_t *)address, sizeof(uint16_t),
                                            (xfer->dataSize), kDMA_MemoryToPeripheral, NULL);
                    }
                    else
                    {
                        DMA_PrepareTransfer(&xferConfig, xfer->txData, (uint32_t *)address, sizeof(uint8_t),
                                            (xfer->dataSize), kDMA_MemoryToPeripheral, NULL);
                    }
                }
            }
            result = DMA_SubmitTransfer(handle->txHandle, &xferConfig);
            if (result != kStatus_Success)
            {
                return result;
            }
        }
        else
        {
            /* Create chained descriptor to transmit dummy word. */
            SPI_SetupDummy(base, &s_txDummy[instance], xfer, spi_config_p);

            if (((xfer->configFlags & (uint32_t)kSPI_FrameAssert) != 0U) &&
                (((uint32_t)spi_config_p->dataWidth > 7U) ? (xfer->dataSize > 2U) : (xfer->dataSize > 1U)))
            {
                dma_xfercfg_t tmp_xfercfg = {0};
                tmp_xfercfg.valid         = true;
                tmp_xfercfg.swtrig        = true;
                tmp_xfercfg.intA          = true;
                tmp_xfercfg.byteWidth     = sizeof(uint32_t);
                tmp_xfercfg.srcInc        = 0;
                tmp_xfercfg.dstInc        = 0;
                tmp_xfercfg.transferCount = 1;

                if (SPI_IsTxFifoEnabled(base))
                {
                    address = (uint32_t) & (VFIFO->SPI[instance].TXDATSPI);
                }
                else
                {
                    address = (uint32_t)&base->TXDATCTL;
                }
                DMA_CreateDescriptor(&s_spi_descriptor_table[instance], &tmp_xfercfg, &s_txDummy[instance].lastWord,
                                     (uint32_t *)address, NULL);
                DMA_PrepareTransfer(
                    &xferConfig, &s_txDummy[instance].word, (uint32_t *)address,
                    (((uint32_t)spi_config_p->dataWidth > 7U) ? (sizeof(uint16_t)) : (sizeof(uint8_t))),
                    (((uint32_t)spi_config_p->dataWidth > 7U) ? (xfer->dataSize - 2U) : (xfer->dataSize - 1U)),
                    kDMA_StaticToStatic, &s_spi_descriptor_table[instance]);
                /* Disable interrupts for first descriptor to avoid calling callback twice */
                xferConfig.xfercfg.intA = false;
                xferConfig.xfercfg.intB = false;
                result                  = DMA_SubmitTransfer(handle->txHandle, &xferConfig);
                if (result != kStatus_Success)
                {
                    return result;
                }
            }
            else
            {
                if (SPI_IsTxFifoEnabled(base))
                {
                    address = (uint32_t) & (VFIFO->SPI[instance].TXDATSPI);
                }
                else
                {
                    address = (uint32_t)&base->TXDAT;
                }
                DMA_PrepareTransfer(&xferConfig, &s_txDummy[instance].word, (uint32_t *)address,
                                    (((uint32_t)spi_config_p->dataWidth > 7U) ? (sizeof(uint16_t)) : (sizeof(uint8_t))),
                                    (xfer->dataSize), kDMA_StaticToStatic, NULL);
                result = DMA_SubmitTransfer(handle->txHandle, &xferConfig);
                if (result != kStatus_Success)
                {
                    return result;
                }
            }
        }
        handle->txInProgress = true;
        /* Setup the control information. */
        if (SPI_IsTxFifoEnabled(base))
        {
            *((uint16_t *)((uint32_t) & (VFIFO->SPI[instance].TXDATSPI)) + 1U) = (uint16_t)(tmp >> 16U);
        }
        else
        {
            base->TXCTL = tmp;
        }
        DMA_StartTransfer(handle->txHandle);
    }

    return result;
}

/*!
 * brief Transfers a block of data using a DMA method.
 *
 * This function using polling way to do the first half transimission and using DMA way to
 * do the srcond half transimission, the transfer mechanism is half-duplex.
 * When do the second half transimission, code will return right away. When all data is transferred,
 * the callback function is called.
 *
 * param base SPI base pointer
 * param handle A pointer to the spi_master_dma_handle_t structure which stores the transfer state.
 * param transfer A pointer to the spi_half_duplex_transfer_t structure.
 * return status of status_t.
 */
status_t SPI_MasterHalfDuplexTransferDMA(SPI_Type *base, spi_dma_handle_t *handle, spi_half_duplex_transfer_t *xfer)
{
    assert(xfer != NULL);
    assert(handle != NULL);
    spi_transfer_t tempXfer = {0};
    status_t status;

    if (xfer->isTransmitFirst)
    {
        tempXfer.txData   = xfer->txData;
        tempXfer.rxData   = NULL;
        tempXfer.dataSize = xfer->txDataSize;
    }
    else
    {
        tempXfer.txData   = NULL;
        tempXfer.rxData   = xfer->rxData;
        tempXfer.dataSize = xfer->rxDataSize;
    }
    /* If the pcs pin keep assert between transmit and receive. */
    if (xfer->isPcsAssertInTransfer)
    {
        tempXfer.configFlags = (xfer->configFlags) & (~(uint32_t)kSPI_FrameAssert);
    }
    else
    {
        tempXfer.configFlags = (xfer->configFlags) | (uint32_t)kSPI_FrameAssert;
    }

    status = SPI_MasterTransferBlocking(base, &tempXfer);
    if (status != kStatus_Success)
    {
        return status;
    }

    if (xfer->isTransmitFirst)
    {
        tempXfer.txData   = NULL;
        tempXfer.rxData   = xfer->rxData;
        tempXfer.dataSize = xfer->rxDataSize;
    }
    else
    {
        tempXfer.txData   = xfer->txData;
        tempXfer.rxData   = NULL;
        tempXfer.dataSize = xfer->txDataSize;
    }
    tempXfer.configFlags = xfer->configFlags;

    status = SPI_MasterTransferDMA(base, handle, &tempXfer);

    return status;
}

static void SPI_RxDMACallback(dma_handle_t *handle, void *userData, bool transferDone, uint32_t intmode)
{
    spi_dma_private_handle_t *privHandle = (spi_dma_private_handle_t *)userData;
    spi_dma_handle_t *spiHandle          = privHandle->handle;
    SPI_Type *base                       = privHandle->base;

    /* change the state */
    spiHandle->rxInProgress = false;

    /* All finished, call the callback */
    if ((spiHandle->txInProgress == false) && (spiHandle->rxInProgress == false))
    {
        spiHandle->state = (uint32_t)kSPI_Idle;
        if (spiHandle->callback != NULL)
        {
            (spiHandle->callback)(base, spiHandle, kStatus_Success, spiHandle->userData);
        }
    }
}

static void SPI_TxDMACallback(dma_handle_t *handle, void *userData, bool transferDone, uint32_t intmode)
{
    spi_dma_private_handle_t *privHandle = (spi_dma_private_handle_t *)userData;
    spi_dma_handle_t *spiHandle          = privHandle->handle;
    SPI_Type *base                       = privHandle->base;
    /* change the state */
    spiHandle->txInProgress = false;
    /* All finished, call the callback */
    if ((spiHandle->txInProgress == false) && (spiHandle->rxInProgress == false))
    {
        spiHandle->state = (uint32_t)kSPI_Idle;
        if (spiHandle->callback != NULL)
        {
            (spiHandle->callback)(base, spiHandle, kStatus_Success, spiHandle->userData);
        }
    }
}

/*!
 * brief Abort a SPI transfer using DMA.
 *
 * param base SPI peripheral base address.
 * param handle SPI DMA handle pointer.
 */
void SPI_MasterTransferAbortDMA(SPI_Type *base, spi_dma_handle_t *handle)
{
    assert(NULL != handle);

    /* Stop tx transfer first */
    DMA_AbortTransfer(handle->txHandle);
    /* Then rx transfer */
    DMA_AbortTransfer(handle->rxHandle);

    /* Set the handle state */
    handle->txInProgress = false;
    handle->rxInProgress = false;
    handle->state        = (uint32_t)kSPI_Idle;
}

/*!
 * brief Gets the master DMA transfer remaining bytes.
 *
 * This function gets the master DMA transfer remaining bytes.
 *
 * param base SPI peripheral base address.
 * param handle A pointer to the spi_dma_handle_t structure which stores the transfer state.
 * param count A number of bytes transferred by the non-blocking transaction.
 * return status of status_t.
 */
status_t SPI_MasterTransferGetCountDMA(SPI_Type *base, spi_dma_handle_t *handle, size_t *count)
{
    assert(handle != NULL);

    if (NULL == count)
    {
        return kStatus_InvalidArgument;
    }

    /* Catch when there is not an active transfer. */
    if (handle->state != (uint32_t)kSPI_Busy)
    {
        *count = 0;
        return kStatus_NoTransferInProgress;
    }

    size_t bytes;

    bytes = DMA_GetRemainingBytes(handle->rxHandle->base, handle->rxHandle->channel);

    *count = handle->transferSize - bytes;

    return kStatus_Success;
}
