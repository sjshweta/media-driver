/*
* Copyright (c) 2017, Intel Corporation
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included
* in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
* OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
* OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
* ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
* OTHER DEALINGS IN THE SOFTWARE.
*/
//!
//! \file      cm_hal_g10.cpp  
//! \brief     Common HAL CM Gen10 functions  
//!

#include "cm_hal_g10.h"
#include "mhw_render_hwcmd_g10_X.h"
#include "mhw_state_heap_hwcmd_g10_X.h"
#include "cm_def.h"
#include "cm_gpucopy_kernel_g10.h" 
#include "cm_gpuinit_kernel_g10.h"
#include "renderhal_platform_interface.h"
#include "mhw_render.h"

// Gen10 Surface state tokenized commands - a SURFACE_STATE_G10 command and
// a surface state command, either SURFACE_STATE_G10 or SURFACE_STATE_ADV_G10
struct PACKET_SURFACE_STATE
{
    SURFACE_STATE_TOKEN_COMMON Token;
    union
    {
        mhw_state_heap_g10_X::RENDER_SURFACE_STATE_CMD cmdSurfaceState;
        mhw_state_heap_g10_X::MEDIA_SURFACE_STATE_CMD cmdSurfaceState_adv;
    };
};

MOS_STATUS CM_HAL_G10_X::SubmitCommands(
    PMHW_BATCH_BUFFER       pBatchBuffer,       
    int32_t                 iTaskId,            
    PCM_HAL_KERNEL_PARAM    *pKernels,          
    void                    **ppCmdBuffer) 
{
    MOS_STATUS                   hr              = MOS_STATUS_SUCCESS;
    PCM_HAL_STATE                pState          = m_cmState;            
    PRENDERHAL_INTERFACE         pRenderHal      = pState->renderHal;
    PMOS_INTERFACE               pOsInterface    = pRenderHal->pOsInterface;
    MhwRenderInterface           *pMhwRender     = pRenderHal->pMhwRenderInterface;
    PMHW_MI_INTERFACE            pMhwMiInterface = pRenderHal->pMhwMiInterface;
    PRENDERHAL_STATE_HEAP        pStateHeap      = pRenderHal->pStateHeap;
    MHW_PIPE_CONTROL_PARAMS      PipeCtlParams   = g_cRenderHal_InitPipeControlParams;
    MHW_MEDIA_STATE_FLUSH_PARAM  FlushParam      = g_cRenderHal_InitMediaStateFlushParams;
    MHW_ID_LOAD_PARAMS           IdLoadParams;
    int32_t                      iRemaining      = 0;
    bool                         enableWalker    = pState->walkerParams.CmWalkerEnable;
    bool                         enableGpGpu     = pState->taskParam->blGpGpuWalkerEnabled;
    PCM_HAL_TASK_PARAM           pTaskParam = pState->taskParam;
    PCM_HAL_BB_ARGS              pBbCmArgs;
    MOS_COMMAND_BUFFER           CmdBuffer;
    uint32_t                     dwSyncTag;
    uint32_t                     dwFrameId;
    int64_t                      *pTaskSyncLocation;
    int32_t                      iSyncOffset;
    int32_t                      iTmp;
    bool                         sip_enable = pRenderHal->bSIPKernel? true: false;
    bool                         csr_enable = pRenderHal->bCSRKernel ? true : false;
	
    uint32_t                     i;
    RENDERHAL_GENERIC_PROLOG_PARAMS genericPrologParams;
    MOS_RESOURCE                 OsResource;

    MOS_ZeroMemory(&CmdBuffer, sizeof(MOS_COMMAND_BUFFER));
    MOS_ZeroMemory(&genericPrologParams, sizeof(genericPrologParams));

    // Get the task sync offset
    iSyncOffset     = pState->pfnGetTaskSyncLocation(iTaskId);

    // Initialize the location
    pTaskSyncLocation                 = (int64_t*)(pState->renderTimeStampResource.data + iSyncOffset);
    *pTaskSyncLocation                = CM_INVALID_INDEX;
    *(pTaskSyncLocation + 1)          = CM_INVALID_INDEX;
    if(pState->cbbEnabled)
    {
        *(pTaskSyncLocation + 2) = CM_INVALID_TAG;
    }

    // Register batch buffer for rendering
    if (!enableWalker && !enableGpGpu)
    {
        CM_CHK_MOSSTATUS(pOsInterface->pfnRegisterResource(
            pOsInterface,
            &pBatchBuffer->OsResource,
            true,
            true));
    }

    // Register Timestamp Buffer
    CM_CHK_MOSSTATUS(pOsInterface->pfnRegisterResource(
        pOsInterface,
        &pState->renderTimeStampResource.osResource,
        true,
        true));

    // Allocate all available space, unused buffer will be returned later
    CM_HRESULT2MOSSTATUS_AND_CHECK(pOsInterface->pfnGetCommandBuffer(pOsInterface, &CmdBuffer, 0));
    iRemaining = CmdBuffer.iRemaining;

    // use frame tracking to write the GPU status Tag to GPU status buffer.
    // On Linux, it just returns next sync tag here since currently no frame tracking support
    dwFrameId = pRenderHal->pfnEnableFrameTracking(pRenderHal, pOsInterface->CurrentGpuContextOrdinal, &genericPrologParams, &OsResource);
    pStateHeap->pCurMediaState->dwSyncTag = dwFrameId;

    // Initialize command buffer and insert prolog
    CM_CHK_MOSSTATUS(pRenderHal->pfnInitCommandBuffer(pRenderHal, &CmdBuffer, &genericPrologParams));

    //Send the First PipeControl Command to indicate the beginning of execution
    PipeCtlParams = g_cRenderHal_InitPipeControlParams;
    PipeCtlParams.presDest          = &pState->renderTimeStampResource.osResource;
    PipeCtlParams.dwResourceOffset  = iSyncOffset;
    PipeCtlParams.dwPostSyncOp      = MHW_FLUSH_WRITE_TIMESTAMP_REG;
    PipeCtlParams.dwFlushMode       = MHW_FLUSH_WRITE_CACHE;
    CM_CHK_MOSSTATUS(pMhwMiInterface->AddPipeControl(&CmdBuffer, nullptr, &PipeCtlParams));

    // Use pipe control to write GPU status tag to CM TS resource so can compare against tag in GPU status buffer
    CM_CHK_MOSSTATUS(pState->pfnWriteGPUStatusTagToCMTSResource(pState, &CmdBuffer, iTaskId, false));

    // update GPU sync tag
    pRenderHal->pfnIncNextFrameId(pRenderHal, pOsInterface->CurrentGpuContextOrdinal);

    // Increment sync tag
    dwSyncTag = pStateHeap->dwNextTag++;

    //enable CNL L3 config
    HalCm_GetLegacyRenderHalL3Setting( &pState->l3Settings, &pRenderHal->L3CacheSettings );
    pRenderHal->pfnEnableL3Caching(pRenderHal, &pRenderHal->L3CacheSettings);
    pMhwRender->SetL3Cache(&CmdBuffer);

    if (sip_enable)
    {
        CM_CHK_MOSSTATUS(SetupHwDebugControl(pRenderHal, &CmdBuffer));
    }

    // add granularity control for preemption for CNL
    // Supporting Preemption granularity control reg for 3D and GPGPU mode for per ctx and with non-privileged access
    if (MEDIA_IS_SKU(pState->skuTable, FtrPerCtxtPreemptionGranularityControl))
    {
        MHW_MI_LOAD_REGISTER_IMM_PARAMS loadRegImm;
        MOS_ZeroMemory(&loadRegImm, sizeof(MHW_MI_LOAD_REGISTER_IMM_PARAMS));

        loadRegImm.dwRegister = MHW_RENDER_ENGINE_PREEMPTION_CONTROL_OFFSET;

        // Same reg offset and value for gpgpu pipe and media pipe
        if (enableGpGpu)
        {
            if (MEDIA_IS_SKU(pState->skuTable, FtrGpGpuMidThreadLevelPreempt))
            {
                if (csr_enable)
                    loadRegImm.dwData = MHW_RENDER_ENGINE_MID_THREAD_PREEMPT_VALUE;
                else
                    loadRegImm.dwData = MHW_RENDER_ENGINE_THREAD_GROUP_PREEMPT_VALUE;
            }
            else if (MEDIA_IS_SKU(pState->skuTable, FtrGpGpuThreadGroupLevelPreempt))
            {
                loadRegImm.dwData = MHW_RENDER_ENGINE_THREAD_GROUP_PREEMPT_VALUE;
                pState->renderHal->pfnEnableGpgpuMiddleBatchBufferPreemption(pState->renderHal);
            }
            else if (MEDIA_IS_SKU(pState->skuTable, FtrGpGpuMidBatchPreempt))
            {
                loadRegImm.dwData = MHW_RENDER_ENGINE_MID_BATCH_PREEMPT_VALUE;
                pState->renderHal->pfnEnableGpgpuMiddleBatchBufferPreemption(pState->renderHal);
            }
            else
            {
                // if hit this branch then platform does not support any media preemption in render engine. Still program the register to avoid GPU hang
                loadRegImm.dwData = MHW_RENDER_ENGINE_MID_BATCH_PREEMPT_VALUE;
            }
        }
        else
        {
            if (MEDIA_IS_SKU(pState->skuTable, FtrMediaMidThreadLevelPreempt))
            {
                loadRegImm.dwData = MHW_RENDER_ENGINE_MID_THREAD_PREEMPT_VALUE;
            }
            else if (MEDIA_IS_SKU(pState->skuTable, FtrMediaThreadGroupLevelPreempt))
            {
                loadRegImm.dwData = MHW_RENDER_ENGINE_THREAD_GROUP_PREEMPT_VALUE;
            }
            else if (MEDIA_IS_SKU(pState->skuTable, FtrMediaMidBatchPreempt))
            {
                loadRegImm.dwData = MHW_RENDER_ENGINE_MID_BATCH_PREEMPT_VALUE;
            }
            else
            {
                // if hit this branch then platform does not support any media preemption in render engine. Still program the register to avoid GPU hang
                loadRegImm.dwData = MHW_RENDER_ENGINE_MID_BATCH_PREEMPT_VALUE;
            }
        }
        CM_CHK_MOSSTATUS(pMhwMiInterface->AddMiLoadRegisterImmCmd(&CmdBuffer, &loadRegImm));
    }

    // Send Pipeline Select command
    CM_CHK_MOSSTATUS(pMhwRender->AddPipelineSelectCmd(&CmdBuffer, enableGpGpu));

    // Send State Base Address command
    CM_CHK_MOSSTATUS(pRenderHal->pfnSendStateBaseAddress(pRenderHal, &CmdBuffer));

    // Send Surface States
    CM_CHK_MOSSTATUS(pRenderHal->pfnSendSurfaces(pRenderHal, &CmdBuffer));

	if (enableGpGpu) {
		if (csr_enable) {

			// Send CS_STALL pipe control
			//Insert a pipe control as synchronization
			PipeCtlParams = g_cRenderHal_InitPipeControlParams;
			PipeCtlParams.presDest = &pState->renderTimeStampResource.osResource;
			PipeCtlParams.dwPostSyncOp = MHW_FLUSH_NOWRITE;
			PipeCtlParams.dwFlushMode = MHW_FLUSH_WRITE_CACHE;
			PipeCtlParams.bDisableCSStall = 0;
			CM_CHK_MOSSTATUS(pMhwMiInterface->AddPipeControl(&CmdBuffer, nullptr, &PipeCtlParams));
		}

		if (sip_enable || csr_enable)
		{
			// Send SIP State
			CM_CHK_MOSSTATUS(pRenderHal->pfnSendSipStateCmd(pRenderHal, &CmdBuffer));

			CM_HRESULT2MOSSTATUS_AND_CHECK(pOsInterface->pfnRegisterResource(
				pOsInterface,
				&pState->csrResource,
				true,
				true));

			// Send csr base addr command
			CM_CHK_MOSSTATUS(pMhwRender->AddGpgpuCsrBaseAddrCmd(&CmdBuffer, &pState->csrResource));
		}
	}

    // Setup VFE State params. Each Renderer MUST call pfnSetVfeStateParams().
    // See comment in VpHal_HwSetVfeStateParams() for details.
    iTmp = RENDERHAL_USE_MEDIA_THREADS_MAX;
    if (pState->maxHWThreadValues.userFeatureValue != 0)
    {
        if( pState->maxHWThreadValues.userFeatureValue < pRenderHal->pHwCaps->dwMaxThreads)
        {
            iTmp = pState->maxHWThreadValues.userFeatureValue;
        }
    }
    else if (pState->maxHWThreadValues.apiValue != 0)
    {
        if( pState->maxHWThreadValues.apiValue < pRenderHal->pHwCaps->dwMaxThreads)
        {
            iTmp = pState->maxHWThreadValues.apiValue;
        }
    }

    pRenderHal->pfnSetVfeStateParams(
        pRenderHal,
        MEDIASTATE_DEBUG_COUNTER_FREE_RUNNING,
        iTmp,
        pState->taskParam->vfeCurbeSize,
        pState->taskParam->urbEntrySize,
        &pState->scoreboardParams);

    // Send VFE State
    CM_CHK_MOSSTATUS(pMhwRender->AddMediaVfeCmd(&CmdBuffer, 
                     pRenderHal->pRenderHalPltInterface->GetVfeStateParameters()));

    // Send CURBE Load
    if (pState->taskParam->vfeCurbeSize > 0)
    {
        CM_CHK_MOSSTATUS(pRenderHal->pfnSendCurbeLoad(pRenderHal, &CmdBuffer));
    }

    // Send Interface Descriptor Load
    if (pState->dshEnabled)
    {
        PRENDERHAL_DYNAMIC_STATE pDynamicState = pStateHeap->pCurMediaState->pDynamicState;
        IdLoadParams.dwInterfaceDescriptorStartOffset = pDynamicState->pMemoryBlock->dwDataOffset +
                                                        pDynamicState->MediaID.dwOffset;
        IdLoadParams.dwInterfaceDescriptorLength      = pDynamicState->MediaID.iCount * pStateHeap->dwSizeMediaID;
    }
    else
    {
        IdLoadParams.dwInterfaceDescriptorStartOffset = pStateHeap->pCurMediaState->dwOffset + pStateHeap->dwOffsetMediaID;
        IdLoadParams.dwInterfaceDescriptorLength      = pRenderHal->StateHeapSettings.iMediaIDs * pStateHeap->dwSizeMediaID;
    }
    IdLoadParams.pKernelState = nullptr;
    CM_CHK_MOSSTATUS(pMhwRender->AddMediaIDLoadCmd(&CmdBuffer, &IdLoadParams));

    if (enableWalker)
    {
        // send media walker command, if required
       
        for (uint32_t i = 0; i < pState->taskParam->numKernels; i ++)
        {
            // Insert CONDITIONAL_BATCH_BUFFER_END
            if ( pTaskParam->conditionalEndBitmap & ((uint64_t)1 << (i)) )
            {
                // this could be batch buffer end so need to update sync tag, media state flush, write end timestamp

                CM_CHK_MOSSTATUS(pRenderHal->pfnSendSyncTag(pRenderHal, &CmdBuffer));

                // Insert a pipe control for synchronization since this Conditional Batch Buffer End command 
                // will use value written by previous kernel. Also needed since this may be the Batch Buffer End
                PipeCtlParams = g_cRenderHal_InitPipeControlParams;
                PipeCtlParams.presDest = &pState->renderTimeStampResource.osResource;
                PipeCtlParams.dwPostSyncOp = MHW_FLUSH_NOWRITE;
                PipeCtlParams.dwFlushMode = MHW_FLUSH_WRITE_CACHE;
                CM_CHK_MOSSTATUS(pMhwMiInterface->AddPipeControl(&CmdBuffer, nullptr, &PipeCtlParams));

                // issue a PIPE_CONTROL to write timestamp
                PipeCtlParams = g_cRenderHal_InitPipeControlParams;
                PipeCtlParams.presDest = &pState->renderTimeStampResource.osResource;
                PipeCtlParams.dwResourceOffset = iSyncOffset + sizeof(uint64_t);
                PipeCtlParams.dwPostSyncOp = MHW_FLUSH_WRITE_TIMESTAMP_REG;
                PipeCtlParams.dwFlushMode = MHW_FLUSH_READ_CACHE;
                CM_CHK_MOSSTATUS(pMhwMiInterface->AddPipeControl(&CmdBuffer, nullptr, &PipeCtlParams));

                // Insert conditional batch buffer end
                pMhwMiInterface->AddMiConditionalBatchBufferEndCmd(&CmdBuffer, &pTaskParam->conditionalBBEndParams[i]);
            }

            //Insert PIPE_CONTROL at two cases:
            // 1. synchronization is set
            // 2. the next kernel has dependency pattern
            if((i > 0) && ((pTaskParam->syncBitmap & ((uint64_t)1 << (i-1))) || 
                (pKernels[i]->kernelThreadSpaceParam.patternType != CM_NONE_DEPENDENCY)))
            {
                //Insert a pipe control as synchronization
                PipeCtlParams = g_cRenderHal_InitPipeControlParams;
                PipeCtlParams.presDest = &pState->renderTimeStampResource.osResource;
                PipeCtlParams.dwPostSyncOp = MHW_FLUSH_NOWRITE;
                PipeCtlParams.dwFlushMode = MHW_FLUSH_CUSTOM;
                PipeCtlParams.bInvalidateTextureCache = true;
                PipeCtlParams.bFlushRenderTargetCache = true;
                CM_CHK_MOSSTATUS(pMhwMiInterface->AddPipeControl(&CmdBuffer, nullptr, &PipeCtlParams));
            }

            // send media walker command, if required
            CM_CHK_MOSSTATUS(pState->pfnSendMediaWalkerState(pState, pKernels[i], &CmdBuffer));
        }

    }
    else if (enableGpGpu)
    {
        // send GPGPU walker command, if required

        for (uint32_t i = 0; i < pState->taskParam->numKernels; i ++)
        {
            MHW_GPGPU_WALKER_PARAMS GpGpuWalkerParams;

            GpGpuWalkerParams.InterfaceDescriptorOffset = pKernels[i]->gpgpuWalkerParams.interfaceDescriptorOffset;
            GpGpuWalkerParams.GpGpuEnable               = pKernels[i]->gpgpuWalkerParams.gpgpuEnabled;
            GpGpuWalkerParams.GroupWidth                = pKernels[i]->gpgpuWalkerParams.groupWidth;
            GpGpuWalkerParams.GroupHeight               = pKernels[i]->gpgpuWalkerParams.groupHeight;
			GpGpuWalkerParams.GroupDepth                = pKernels[i]->gpgpuWalkerParams.groupDepth;
            GpGpuWalkerParams.ThreadWidth               = pKernels[i]->gpgpuWalkerParams.threadWidth;
            GpGpuWalkerParams.ThreadHeight              = pKernels[i]->gpgpuWalkerParams.threadHeight;
			GpGpuWalkerParams.ThreadDepth               = pKernels[i]->gpgpuWalkerParams.threadDepth;
            GpGpuWalkerParams.SLMSize                   = pKernels[i]->slmSize;
            //Insert PIPE_CONTROL at two cases:
            // 1. synchronization is set
            // 2. the next kernel has dependency pattern
            if((i > 0) && ((pTaskParam->syncBitmap & ((uint64_t)1 << (i-1))) || 
                (pKernels[i]->kernelThreadSpaceParam.patternType != CM_NONE_DEPENDENCY)))
            {
                //Insert a pipe control as synchronization
                PipeCtlParams = g_cRenderHal_InitPipeControlParams;
                PipeCtlParams.presDest = &pState->renderTimeStampResource.osResource;
                PipeCtlParams.dwPostSyncOp = MHW_FLUSH_NOWRITE;
                PipeCtlParams.dwFlushMode = MHW_FLUSH_CUSTOM;
                PipeCtlParams.bInvalidateTextureCache = true;
                PipeCtlParams.bFlushRenderTargetCache = true;
                CM_CHK_MOSSTATUS(pMhwMiInterface->AddPipeControl(&CmdBuffer, nullptr, &PipeCtlParams));
            }

           // CM_CHK_STATUS(HalCm_SendWalkerState_g10(pState, pKernels[i], &CmdBuffer));
        // send media walker command, if required
         MOS_SecureMemcpy(&pState->walkerParams, sizeof(MHW_WALKER_PARAMS), &pKernels[i]->walkerParams, sizeof(CM_HAL_WALKER_PARAMS));
         CM_CHK_MOSSTATUS(pMhwRender->AddGpGpuWalkerStateCmd(&CmdBuffer, &GpGpuWalkerParams));
        }
        
    }
    else
    {
        // Send Start batch buffer command
        CM_CHK_MOSSTATUS(pMhwMiInterface->AddMiBatchBufferStartCmd(
            &CmdBuffer, 
            pBatchBuffer));

        CM_CHK_NULL_RETURN_MOSSTATUS(pBatchBuffer->pPrivateData);
        pBbCmArgs = (PCM_HAL_BB_ARGS) pBatchBuffer->pPrivateData;

        if ( (pBbCmArgs->refCount == 1) ||
                 (pState->taskParam->reuseBBUpdateMask == 1) )
        {
            CM_CHK_MOSSTATUS(pMhwMiInterface->AddMiBatchBufferEnd(nullptr, pBatchBuffer));
        }
        else
        {
            // Skip BB end command
            CM_CHK_MOSSTATUS(pMhwMiInterface->SkipMiBatchBufferEndBb(pBatchBuffer));
        }

        // UnLock the batch buffer
        if ( (pBbCmArgs->refCount == 1) ||
             (pState->taskParam->reuseBBUpdateMask == 1) )
        {
            CM_CHK_MOSSTATUS(pRenderHal->pfnUnlockBB(pRenderHal, pBatchBuffer));
        }
    }

    // issue a PIPE_CONTROL to flush all caches and the stall the CS before 
    // issuing a PIPE_CONTROL to write the timestamp
    PipeCtlParams = g_cRenderHal_InitPipeControlParams;
    PipeCtlParams.presDest      = &pState->renderTimeStampResource.osResource;
    PipeCtlParams.dwPostSyncOp  = MHW_FLUSH_NOWRITE;
    PipeCtlParams.dwFlushMode   = MHW_FLUSH_WRITE_CACHE;
    CM_CHK_MOSSTATUS(pMhwMiInterface->AddPipeControl(&CmdBuffer, nullptr, &PipeCtlParams));

    // Find the SVM slot, patch it into this dummy pipe_control
    for (i = 0; i < pState->cmDeviceParam.maxBufferTableSize; i++)
    {
        //Only register SVM resource here
        if (pState->bufferTable[i].address)
        {
            CM_HRESULT2MOSSTATUS_AND_CHECK(pOsInterface->pfnRegisterResource(
                    pOsInterface,
                    &pState->bufferTable[i].osResource,
                    true,
                    false));
        }
    }

    // issue a PIPE_CONTROL to write timestamp
    iSyncOffset += sizeof(uint64_t);
    PipeCtlParams = g_cRenderHal_InitPipeControlParams;
    PipeCtlParams.presDest          = &pState->renderTimeStampResource.osResource;
    PipeCtlParams.dwResourceOffset  = iSyncOffset;
    PipeCtlParams.dwPostSyncOp      = MHW_FLUSH_WRITE_TIMESTAMP_REG;
    PipeCtlParams.dwFlushMode       = MHW_FLUSH_READ_CACHE;
    CM_CHK_MOSSTATUS(pMhwMiInterface->AddPipeControl(&CmdBuffer, nullptr, &PipeCtlParams));

    // Send Sync Tag 
    CM_CHK_MOSSTATUS( pRenderHal->pfnSendSyncTag( pRenderHal, &CmdBuffer ) );

    // Add PipeControl to invalidate ISP and MediaState to avoid PageFault issue
    MHW_PIPE_CONTROL_PARAMS PipeControlParams;

    MOS_ZeroMemory(&PipeControlParams, sizeof(PipeControlParams));
    PipeControlParams.dwFlushMode = MHW_FLUSH_WRITE_CACHE;
    PipeControlParams.bGenericMediaStateClear = true;
    PipeControlParams.bIndirectStatePointersDisable = true;
    PipeControlParams.bDisableCSStall = false;
    CM_CHK_MOSSTATUS(pMhwMiInterface->AddPipeControl(&CmdBuffer, nullptr, &PipeControlParams));

    //Couple to the BB_START , otherwise GPU Hang without it in KMD.
    CM_CHK_MOSSTATUS(pMhwMiInterface->AddMiBatchBufferEnd(&CmdBuffer, nullptr));

    // Return unused command buffer space to OS
    pOsInterface->pfnReturnCommandBuffer(pOsInterface, &CmdBuffer, 0);

#if MDF_COMMAND_BUFFER_DUMP
    if (pState->dumpCommandBuffer)
    {
        pState->pfnDumpCommadBuffer(
            pState,
            &CmdBuffer,
            offsetof(PACKET_SURFACE_STATE, cmdSurfaceState),
            mhw_state_heap_g10_X::RENDER_SURFACE_STATE_CMD::byteSize);
    }
#endif

    CM_CHK_MOSSTATUS(pState->pfnGetGlobalTime(&pState->taskTimeStamp->submitTimeInCpu[iTaskId]));
    CM_CHK_MOSSTATUS(pState->pfnGetGpuTime(pState, &pState->taskTimeStamp->submitTimeInGpu[iTaskId]));

    // Submit command buffer
    CM_HRESULT2MOSSTATUS_AND_CHECK(pOsInterface->pfnSubmitCommandBuffer(pOsInterface,
        &CmdBuffer,
        pState->nullHwRenderCm));

    if (pState->nullHwRenderCm == false)
    {
        pStateHeap->pCurMediaState->bBusy = true;
        if ( !enableWalker && !enableGpGpu )
        {
            pBatchBuffer->bBusy     = true;
            pBatchBuffer->dwSyncTag = dwSyncTag;
        }
    }

    // reset API call number of HW threads
    pState->maxHWThreadValues.apiValue = 0;

    pState->pfnReferenceCommandBuffer(&CmdBuffer.OsResource, ppCmdBuffer);

    hr = MOS_STATUS_SUCCESS;

finish:
    // Failed -> discard all changes in Command Buffer
    if (hr != MOS_STATUS_SUCCESS)
    {
        // Buffer overflow - display overflow size
        if (CmdBuffer.iRemaining < 0)
        {
            CM_PUBLIC_ASSERTMESSAGE("Command Buffer overflow by %d bytes.", -CmdBuffer.iRemaining);
        }

        // Move command buffer back to beginning
        iTmp = iRemaining - CmdBuffer.iRemaining;
        CmdBuffer.iRemaining  = iRemaining;
        CmdBuffer.iOffset    -= iTmp;
        CmdBuffer.pCmdPtr     = CmdBuffer.pCmdBase + CmdBuffer.iOffset/sizeof(uint32_t);

        // Return unused command buffer space to OS
        pOsInterface->pfnReturnCommandBuffer(pOsInterface, &CmdBuffer, 0);
    }

    return hr;
}

MOS_STATUS CM_HAL_G10_X::HwSetSurfaceMemoryObjectControl(
    uint16_t                        wMemObjCtl,
    PRENDERHAL_SURFACE_STATE_PARAMS pParams)
{
	PRENDERHAL_INTERFACE pRenderHal = m_cmState->renderHal;
	MOS_STATUS hr = MOS_STATUS_SUCCESS;

	CM_HAL_MEMORY_OBJECT_CONTROL_G9 cache_type;
	// The memory object control uint16_t is composed with cache type(8:15), memory type(4:7), ages(0:3)
	cache_type = (CM_HAL_MEMORY_OBJECT_CONTROL_G9)((wMemObjCtl & CM_MEMOBJCTL_CACHE_MASK) >> 8);
	
	if ((uint16_t)cache_type == CM_INVALID_MEMOBJCTL)
	{
		pParams->MemObjCtl = pRenderHal->pOsInterface->pfnCachePolicyGetMemoryObject(MOS_CM_RESOURCE_USAGE_SurfaceState).DwordValue;
		return hr;
	}

	switch (cache_type)
	{
	case CM_MEMORY_OBJECT_CONTROL_SKL_DEFAULT:
		pParams->MemObjCtl = pRenderHal->pOsInterface->pfnCachePolicyGetMemoryObject(MOS_CM_RESOURCE_USAGE_SurfaceState).DwordValue;
		break;
	case CM_MEMORY_OBJECT_CONTROL_SKL_NO_L3:
		pParams->MemObjCtl = pRenderHal->pOsInterface->pfnCachePolicyGetMemoryObject(MOS_CM_RESOURCE_USAGE_NO_L3_SurfaceState).DwordValue;
		break;
	case CM_MEMORY_OBJECT_CONTROL_SKL_NO_LLC_ELLC:
		pParams->MemObjCtl = pRenderHal->pOsInterface->pfnCachePolicyGetMemoryObject(MOS_CM_RESOURCE_USAGE_NO_LLC_ELLC_SurfaceState).DwordValue;
		break;
	case CM_MEMORY_OBJECT_CONTROL_SKL_NO_LLC:
		pParams->MemObjCtl = pRenderHal->pOsInterface->pfnCachePolicyGetMemoryObject(MOS_CM_RESOURCE_USAGE_NO_LLC_SurfaceState).DwordValue;
		break;
	case CM_MEMORY_OBJECT_CONTROL_SKL_NO_ELLC:
		pParams->MemObjCtl = pRenderHal->pOsInterface->pfnCachePolicyGetMemoryObject(MOS_CM_RESOURCE_USAGE_NO_ELLC_SurfaceState).DwordValue;
		break;
	case CM_MEMORY_OBJECT_CONTROL_SKL_NO_LLC_L3:
		pParams->MemObjCtl = pRenderHal->pOsInterface->pfnCachePolicyGetMemoryObject(MOS_CM_RESOURCE_USAGE_NO_LLC_L3_SurfaceState).DwordValue;
		break;
	case CM_MEMORY_OBJECT_CONTROL_SKL_NO_ELLC_L3:
		pParams->MemObjCtl = pRenderHal->pOsInterface->pfnCachePolicyGetMemoryObject(MOS_CM_RESOURCE_USAGE_NO_ELLC_L3_SurfaceState).DwordValue;
		break;
	case CM_MEMORY_OBJECT_CONTROL_SKL_NO_CACHE:
		pParams->MemObjCtl = pRenderHal->pOsInterface->pfnCachePolicyGetMemoryObject(MOS_CM_RESOURCE_USAGE_NO_CACHE_SurfaceState).DwordValue;	
		break;
	default:
		hr = MOS_STATUS_UNKNOWN;
	}

	return hr;
}

MOS_STATUS CM_HAL_G10_X::UpdatePlatformInfoFromPower(
    PCM_PLATFORM_INFO      platformInfo,
    bool                   bEUSaturation)
{
    UNUSED(platformInfo);
    UNUSED(bEUSaturation);
    // needs to be implemented
    return MOS_STATUS_SUCCESS;
}

MOS_STATUS CM_HAL_G10_X::AllocateSIPCSRResource()
{
    MOS_STATUS hr = MOS_STATUS_SUCCESS;
    if (Mos_ResourceIsNull(&m_cmState->sipResource.osResource)) 
    {
        // create  sip resource if it does not exist
        CM_CHK_MOSSTATUS_RETURN(HalCm_AllocateSipResource(m_cmState)); 
        CM_CHK_MOSSTATUS_RETURN(HalCm_AllocateCSRResource(m_cmState));
    }
    return hr;
}

MOS_STATUS CM_HAL_G10_X::RegisterSampler8x8AVSTable(
   PCM_HAL_SAMPLER_8X8_TABLE  pSampler8x8AVSTable,
   PCM_AVS_TABLE_STATE_PARAMS pAVSTable)
{
    MOS_ZeroMemory(&pSampler8x8AVSTable->mhwSamplerAvsTableParam, sizeof(pSampler8x8AVSTable->mhwSamplerAvsTableParam));

    pSampler8x8AVSTable->mhwSamplerAvsTableParam.byteTransitionArea8Pixels = MEDIASTATE_AVS_TRANSITION_AREA_8_PIXELS;
    pSampler8x8AVSTable->mhwSamplerAvsTableParam.byteTransitionArea4Pixels = MEDIASTATE_AVS_TRANSITION_AREA_4_PIXELS;
    pSampler8x8AVSTable->mhwSamplerAvsTableParam.byteMaxDerivative8Pixels = MEDIASTATE_AVS_MAX_DERIVATIVE_8_PIXELS;
    pSampler8x8AVSTable->mhwSamplerAvsTableParam.byteMaxDerivative4Pixels = MEDIASTATE_AVS_MAX_DERIVATIVE_4_PIXELS;

    pSampler8x8AVSTable->mhwSamplerAvsTableParam.bEnableRGBAdaptive = pAVSTable->enableRgbAdaptive;
    pSampler8x8AVSTable->mhwSamplerAvsTableParam.bAdaptiveFilterAllChannels = pAVSTable->adaptiveFilterAllChannels;

    // Assign the coefficient table;
    for (uint32_t i = 0; i < CM_NUM_HW_POLYPHASE_TABLES_G10; i++)
    {
        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParam[i].ZeroXFilterCoefficient[0] = (uint8_t)pAVSTable->tbl0X[i].FilterCoeff_0_0;
        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParam[i].ZeroXFilterCoefficient[1] = (uint8_t)pAVSTable->tbl0X[i].FilterCoeff_0_1;

        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParam[i].ZeroXFilterCoefficient[2] = (uint8_t)pAVSTable->tbl0X[i].FilterCoeff_0_2;
        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParam[i].ZeroXFilterCoefficient[3] = (uint8_t)pAVSTable->tbl0X[i].FilterCoeff_0_3;

        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParam[i].ZeroXFilterCoefficient[4] = (uint8_t)pAVSTable->tbl0X[i].FilterCoeff_0_4;
        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParam[i].ZeroXFilterCoefficient[5] = (uint8_t)pAVSTable->tbl0X[i].FilterCoeff_0_5;

        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParam[i].ZeroXFilterCoefficient[6] = (uint8_t)pAVSTable->tbl0X[i].FilterCoeff_0_6;
        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParam[i].ZeroXFilterCoefficient[7] = (uint8_t)pAVSTable->tbl0X[i].FilterCoeff_0_7;

        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParam[i].ZeroYFilterCoefficient[0] = (uint8_t)pAVSTable->tbl0Y[i].FilterCoeff_0_0;
        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParam[i].ZeroYFilterCoefficient[1] = (uint8_t)pAVSTable->tbl0Y[i].FilterCoeff_0_1;

        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParam[i].ZeroYFilterCoefficient[2] = (uint8_t)pAVSTable->tbl0Y[i].FilterCoeff_0_2;
        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParam[i].ZeroYFilterCoefficient[3] = (uint8_t)pAVSTable->tbl0Y[i].FilterCoeff_0_3;

        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParam[i].ZeroYFilterCoefficient[4] = (uint8_t)pAVSTable->tbl0Y[i].FilterCoeff_0_4;
        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParam[i].ZeroYFilterCoefficient[5] = (uint8_t)pAVSTable->tbl0Y[i].FilterCoeff_0_5;

        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParam[i].ZeroYFilterCoefficient[6] = (uint8_t)pAVSTable->tbl0Y[i].FilterCoeff_0_6;
        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParam[i].ZeroYFilterCoefficient[7] = (uint8_t)pAVSTable->tbl0Y[i].FilterCoeff_0_7;

        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParam[i].OneXFilterCoefficient[0] = (uint8_t)pAVSTable->tbl1X[i].FilterCoeff_0_2;
        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParam[i].OneXFilterCoefficient[1] = (uint8_t)pAVSTable->tbl1X[i].FilterCoeff_0_3;
        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParam[i].OneXFilterCoefficient[2] = (uint8_t)pAVSTable->tbl1X[i].FilterCoeff_0_4;
        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParam[i].OneXFilterCoefficient[3] = (uint8_t)pAVSTable->tbl1X[i].FilterCoeff_0_5;

        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParam[i].OneYFilterCoefficient[0] = (uint8_t)pAVSTable->tbl1Y[i].FilterCoeff_0_2;
        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParam[i].OneYFilterCoefficient[1] = (uint8_t)pAVSTable->tbl1Y[i].FilterCoeff_0_3;
        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParam[i].OneYFilterCoefficient[2] = (uint8_t)pAVSTable->tbl1Y[i].FilterCoeff_0_4;
        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParam[i].OneYFilterCoefficient[3] = (uint8_t)pAVSTable->tbl1Y[i].FilterCoeff_0_5;
    }

    pSampler8x8AVSTable->mhwSamplerAvsTableParam.byteDefaultSharpnessLevel = pAVSTable->defaultSharpLevel;
    pSampler8x8AVSTable->mhwSamplerAvsTableParam.bBypassXAdaptiveFiltering = pAVSTable->bypassXAF;
    pSampler8x8AVSTable->mhwSamplerAvsTableParam.bBypassYAdaptiveFiltering = pAVSTable->bypassYAF;

    if (!pAVSTable->bypassXAF && !pAVSTable->bypassYAF)
    {
        pSampler8x8AVSTable->mhwSamplerAvsTableParam.byteMaxDerivative8Pixels = pAVSTable->maxDerivative8Pixels;
        pSampler8x8AVSTable->mhwSamplerAvsTableParam.byteMaxDerivative4Pixels = pAVSTable->maxDerivative4Pixels;
        pSampler8x8AVSTable->mhwSamplerAvsTableParam.byteTransitionArea8Pixels = pAVSTable->transitionArea8Pixels;
        pSampler8x8AVSTable->mhwSamplerAvsTableParam.byteTransitionArea4Pixels = pAVSTable->transitionArea4Pixels;
    }

    for (int i = 0; i < CM_NUM_HW_POLYPHASE_EXTRA_TABLES_G10; i++)
    {
        int src = i + CM_NUM_HW_POLYPHASE_TABLES_G10;
        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParamExtra[i].ZeroXFilterCoefficient[0] = (uint8_t)pAVSTable->tbl0X[src].FilterCoeff_0_0;
        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParamExtra[i].ZeroXFilterCoefficient[1] = (uint8_t)pAVSTable->tbl0X[src].FilterCoeff_0_1;

        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParamExtra[i].ZeroXFilterCoefficient[2] = (uint8_t)pAVSTable->tbl0X[src].FilterCoeff_0_2;
        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParamExtra[i].ZeroXFilterCoefficient[3] = (uint8_t)pAVSTable->tbl0X[src].FilterCoeff_0_3;

        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParamExtra[i].ZeroXFilterCoefficient[4] = (uint8_t)pAVSTable->tbl0X[src].FilterCoeff_0_4;
        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParamExtra[i].ZeroXFilterCoefficient[5] = (uint8_t)pAVSTable->tbl0X[src].FilterCoeff_0_5;

        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParamExtra[i].ZeroXFilterCoefficient[6] = (uint8_t)pAVSTable->tbl0X[src].FilterCoeff_0_6;
        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParamExtra[i].ZeroXFilterCoefficient[7] = (uint8_t)pAVSTable->tbl0X[src].FilterCoeff_0_7;

        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParamExtra[i].ZeroYFilterCoefficient[0] = (uint8_t)pAVSTable->tbl0Y[src].FilterCoeff_0_0;
        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParamExtra[i].ZeroYFilterCoefficient[1] = (uint8_t)pAVSTable->tbl0Y[src].FilterCoeff_0_1;

        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParamExtra[i].ZeroYFilterCoefficient[2] = (uint8_t)pAVSTable->tbl0Y[src].FilterCoeff_0_2;
        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParamExtra[i].ZeroYFilterCoefficient[3] = (uint8_t)pAVSTable->tbl0Y[src].FilterCoeff_0_3;

        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParamExtra[i].ZeroYFilterCoefficient[4] = (uint8_t)pAVSTable->tbl0Y[src].FilterCoeff_0_4;
        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParamExtra[i].ZeroYFilterCoefficient[5] = (uint8_t)pAVSTable->tbl0Y[src].FilterCoeff_0_5;

        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParamExtra[i].ZeroYFilterCoefficient[6] = (uint8_t)pAVSTable->tbl0Y[src].FilterCoeff_0_6;
        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParamExtra[i].ZeroYFilterCoefficient[7] = (uint8_t)pAVSTable->tbl0Y[src].FilterCoeff_0_7;

        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParamExtra[i].OneXFilterCoefficient[0] = (uint8_t)pAVSTable->tbl1X[src].FilterCoeff_0_2;
        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParamExtra[i].OneXFilterCoefficient[1] = (uint8_t)pAVSTable->tbl1X[src].FilterCoeff_0_3;
        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParamExtra[i].OneXFilterCoefficient[2] = (uint8_t)pAVSTable->tbl1X[src].FilterCoeff_0_4;
        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParamExtra[i].OneXFilterCoefficient[3] = (uint8_t)pAVSTable->tbl1X[src].FilterCoeff_0_5;

        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParamExtra[i].OneYFilterCoefficient[0] = (uint8_t)pAVSTable->tbl1Y[src].FilterCoeff_0_2;
        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParamExtra[i].OneYFilterCoefficient[1] = (uint8_t)pAVSTable->tbl1Y[src].FilterCoeff_0_3;
        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParamExtra[i].OneYFilterCoefficient[2] = (uint8_t)pAVSTable->tbl1Y[src].FilterCoeff_0_4;
        pSampler8x8AVSTable->mhwSamplerAvsTableParam.paMhwAvsCoeffParamExtra[i].OneYFilterCoefficient[3] = (uint8_t)pAVSTable->tbl1Y[src].FilterCoeff_0_5;

    }

    return MOS_STATUS_SUCCESS;
}

MOS_STATUS CM_HAL_G10_X::RegisterSampler8x8(
	PCM_HAL_SAMPLER_8X8_PARAM    pParam)
{
	PCM_HAL_STATE               pState = m_cmState;
	MOS_STATUS                  hr = MOS_STATUS_SUCCESS;
	int16_t                     samplerIndex = 0;
	PMHW_SAMPLER_STATE_PARAM    pSamplerEntry = nullptr;
	PCM_HAL_SAMPLER_8X8_ENTRY   pSampler8x8Entry = nullptr;

	if (pParam->sampler8x8State.stateType == CM_SAMPLER8X8_AVS)
	{
		for (uint32_t i = 0; i < pState->cmDeviceParam.maxSamplerTableSize; i++) {
			if (!pState->samplerTable[i].bInUse) {
				pSamplerEntry = &pState->samplerTable[i];
				pParam->handle = (uint32_t)i << 16;
				pSamplerEntry->bInUse = true;
				break;
			}
		}

		for (uint32_t i = 0; i < pState->cmDeviceParam.maxSampler8x8TableSize; i++) {
			if (!pState->sampler8x8Table[i].inUse) {
				pSampler8x8Entry = &pState->sampler8x8Table[i];
				samplerIndex = (int16_t)i;
				pParam->handle |= (uint32_t)(i & 0xffff);
				pSampler8x8Entry->inUse = true;
				break;
			}
		}

		if (!pSamplerEntry || !pSampler8x8Entry) {
			CM_ERROR_ASSERT("Sampler or AVS table is full");
			goto finish;
		}

		//State data from application
		pSamplerEntry->SamplerType = MHW_SAMPLER_TYPE_AVS;
		pSamplerEntry->ElementType = MHW_Sampler128Elements;
		pSamplerEntry->Avs = pParam->sampler8x8State.avsParam.avsState;
		pSamplerEntry->Avs.stateID = samplerIndex;
		pSamplerEntry->Avs.iTable8x8_Index = samplerIndex;  // Used for calculating the Media offset of 8x8 table
		pSamplerEntry->Avs.pMhwSamplerAvsTableParam = &pSampler8x8Entry->sampler8x8State.mhwSamplerAvsTableParam;

		if (pSamplerEntry->Avs.EightTapAFEnable)
			pParam->sampler8x8State.avsParam.avsTable.adaptiveFilterAllChannels = true;
		else
			pParam->sampler8x8State.avsParam.avsTable.adaptiveFilterAllChannels = false;

		CM_CHK_MOSSTATUS(RegisterSampler8x8AVSTable(&pSampler8x8Entry->sampler8x8State,
			&pParam->sampler8x8State.avsParam.avsTable));

		pSampler8x8Entry->sampler8x8State.stateType = CM_SAMPLER8X8_AVS;
	}
	else if (pParam->sampler8x8State.stateType == CM_SAMPLER8X8_MISC)
	{
		for (uint32_t i = 0; i < pState->cmDeviceParam.maxSamplerTableSize; i++)
		{
			if (!pState->samplerTable[i].bInUse)
			{
				pSamplerEntry = &pState->samplerTable[i];
				pParam->handle = (uint32_t)i << 16;
				pSamplerEntry->bInUse = true;
				break;
			}
		}

		if (pSamplerEntry == nullptr)
		{
			return MOS_STATUS_INVALID_HANDLE;
		}
		pSamplerEntry->SamplerType = MHW_SAMPLER_TYPE_MISC;
        pSamplerEntry->ElementType = MHW_Sampler2Elements;

		pSamplerEntry->Misc.byteHeight = pParam->sampler8x8State.miscState.DW0.Height;
		pSamplerEntry->Misc.byteWidth = pParam->sampler8x8State.miscState.DW0.Width;
		pSamplerEntry->Misc.wRow[0] = pParam->sampler8x8State.miscState.DW0.Row0;
		pSamplerEntry->Misc.wRow[1] = pParam->sampler8x8State.miscState.DW1.Row1;
		pSamplerEntry->Misc.wRow[2] = pParam->sampler8x8State.miscState.DW1.Row2;
		pSamplerEntry->Misc.wRow[3] = pParam->sampler8x8State.miscState.DW2.Row3;
		pSamplerEntry->Misc.wRow[4] = pParam->sampler8x8State.miscState.DW2.Row4;
		pSamplerEntry->Misc.wRow[5] = pParam->sampler8x8State.miscState.DW3.Row5;
		pSamplerEntry->Misc.wRow[6] = pParam->sampler8x8State.miscState.DW3.Row6;
		pSamplerEntry->Misc.wRow[7] = pParam->sampler8x8State.miscState.DW4.Row7;
		pSamplerEntry->Misc.wRow[8] = pParam->sampler8x8State.miscState.DW4.Row8;
		pSamplerEntry->Misc.wRow[9] = pParam->sampler8x8State.miscState.DW5.Row9;
		pSamplerEntry->Misc.wRow[10] = pParam->sampler8x8State.miscState.DW5.Row10;
		pSamplerEntry->Misc.wRow[11] = pParam->sampler8x8State.miscState.DW6.Row11;
		pSamplerEntry->Misc.wRow[12] = pParam->sampler8x8State.miscState.DW6.Row12;
		pSamplerEntry->Misc.wRow[13] = pParam->sampler8x8State.miscState.DW7.Row13;
		pSamplerEntry->Misc.wRow[14] = pParam->sampler8x8State.miscState.DW7.Row14;
	}
	else if (pParam->sampler8x8State.stateType == CM_SAMPLER8X8_CONV)
	{
		for (uint32_t i = 0; i < pState->cmDeviceParam.maxSamplerTableSize; i++)
		{
			if (!pState->samplerTable[i].bInUse) {
				pSamplerEntry = &pState->samplerTable[i];
				pParam->handle = (uint32_t)i << 16;
				pSamplerEntry->bInUse = true;
				break;
			}
		}

		MOS_ZeroMemory(&pSamplerEntry->Convolve, sizeof(pSamplerEntry->Convolve));

		if (pSamplerEntry == nullptr)
		{
			return MOS_STATUS_INVALID_HANDLE;
		}
		pSamplerEntry->SamplerType = MHW_SAMPLER_TYPE_CONV;

		pSamplerEntry->Convolve.ui8Height = pParam->sampler8x8State.convolveState.height;
		pSamplerEntry->Convolve.ui8Width = pParam->sampler8x8State.convolveState.width;
		pSamplerEntry->Convolve.ui8ScaledDownValue = pParam->sampler8x8State.convolveState.scaleDownValue;
		pSamplerEntry->Convolve.ui8SizeOfTheCoefficient = pParam->sampler8x8State.convolveState.coeffSize;

		pSamplerEntry->Convolve.ui8MSBWidth = pParam->sampler8x8State.convolveState.isHorizontal32Mode;
		pSamplerEntry->Convolve.ui8MSBHeight = pParam->sampler8x8State.convolveState.isVertical32Mode;
		pSamplerEntry->Convolve.skl_mode = pParam->sampler8x8State.convolveState.sklMode;

		// Currently use DW0.Reserved0 to save the detailed Convolve Type, the DW0.Reserved0 will be cleared when copy to sampelr heap
		pSamplerEntry->Convolve.ui8ConvolveType = pParam->sampler8x8State.convolveState.nConvolveType;
		if (pSamplerEntry->Convolve.skl_mode &&
			pSamplerEntry->Convolve.ui8ConvolveType == CM_CONVOLVE_SKL_TYPE_2D)
		{
			pSamplerEntry->ElementType = MHW_Sampler128Elements;
		}
		else if ((!pSamplerEntry->Convolve.skl_mode &&
			pSamplerEntry->Convolve.ui8ConvolveType == CM_CONVOLVE_SKL_TYPE_2D)
			|| pSamplerEntry->Convolve.ui8ConvolveType == CM_CONVOLVE_SKL_TYPE_1P)
		{
			pSamplerEntry->ElementType = MHW_Sampler64Elements;
		}
		else
		{
            pSamplerEntry->ElementType = MHW_Sampler8Elements;
		}

		for (int i = 0; i < CM_NUM_CONVOLVE_ROWS_SKL; i++)
		{
			MHW_SAMPLER_CONVOLVE_COEFF_TABLE *pCoeffTable = &(pSamplerEntry->Convolve.CoeffTable[i]);
			CM_HAL_CONVOLVE_COEFF_TABLE      *pSourceTable = &(pParam->sampler8x8State.convolveState.table[i]);
			if (pSamplerEntry->Convolve.ui8SizeOfTheCoefficient == 1)
			{
				pCoeffTable->wFilterCoeff[0] = FloatToS3_12(pSourceTable->FilterCoeff_0_0);
				pCoeffTable->wFilterCoeff[1] = FloatToS3_12(pSourceTable->FilterCoeff_0_1);
				pCoeffTable->wFilterCoeff[2] = FloatToS3_12(pSourceTable->FilterCoeff_0_2);
				pCoeffTable->wFilterCoeff[3] = FloatToS3_12(pSourceTable->FilterCoeff_0_3);
				pCoeffTable->wFilterCoeff[4] = FloatToS3_12(pSourceTable->FilterCoeff_0_4);
				pCoeffTable->wFilterCoeff[5] = FloatToS3_12(pSourceTable->FilterCoeff_0_5);
				pCoeffTable->wFilterCoeff[6] = FloatToS3_12(pSourceTable->FilterCoeff_0_6);
				pCoeffTable->wFilterCoeff[7] = FloatToS3_12(pSourceTable->FilterCoeff_0_7);
				pCoeffTable->wFilterCoeff[8] = FloatToS3_12(pSourceTable->FilterCoeff_0_8);
				pCoeffTable->wFilterCoeff[9] = FloatToS3_12(pSourceTable->FilterCoeff_0_9);
				pCoeffTable->wFilterCoeff[10] = FloatToS3_12(pSourceTable->FilterCoeff_0_10);
				pCoeffTable->wFilterCoeff[11] = FloatToS3_12(pSourceTable->FilterCoeff_0_11);
				pCoeffTable->wFilterCoeff[12] = FloatToS3_12(pSourceTable->FilterCoeff_0_12);
				pCoeffTable->wFilterCoeff[13] = FloatToS3_12(pSourceTable->FilterCoeff_0_13);
				pCoeffTable->wFilterCoeff[14] = FloatToS3_12(pSourceTable->FilterCoeff_0_14);
				pCoeffTable->wFilterCoeff[15] = FloatToS3_12(pSourceTable->FilterCoeff_0_15);
			}
			else
			{
				pCoeffTable->wFilterCoeff[0] = FloatToS3_4(pSourceTable->FilterCoeff_0_0);
				pCoeffTable->wFilterCoeff[1] = FloatToS3_4(pSourceTable->FilterCoeff_0_1);
				pCoeffTable->wFilterCoeff[2] = FloatToS3_4(pSourceTable->FilterCoeff_0_2);
				pCoeffTable->wFilterCoeff[3] = FloatToS3_4(pSourceTable->FilterCoeff_0_3);
				pCoeffTable->wFilterCoeff[4] = FloatToS3_4(pSourceTable->FilterCoeff_0_4);
				pCoeffTable->wFilterCoeff[5] = FloatToS3_4(pSourceTable->FilterCoeff_0_5);
				pCoeffTable->wFilterCoeff[6] = FloatToS3_4(pSourceTable->FilterCoeff_0_6);
				pCoeffTable->wFilterCoeff[7] = FloatToS3_4(pSourceTable->FilterCoeff_0_7);
				pCoeffTable->wFilterCoeff[8] = FloatToS3_4(pSourceTable->FilterCoeff_0_8);
				pCoeffTable->wFilterCoeff[9] = FloatToS3_4(pSourceTable->FilterCoeff_0_9);
				pCoeffTable->wFilterCoeff[10] = FloatToS3_4(pSourceTable->FilterCoeff_0_10);
				pCoeffTable->wFilterCoeff[11] = FloatToS3_4(pSourceTable->FilterCoeff_0_11);
				pCoeffTable->wFilterCoeff[12] = FloatToS3_4(pSourceTable->FilterCoeff_0_12);
				pCoeffTable->wFilterCoeff[13] = FloatToS3_4(pSourceTable->FilterCoeff_0_13);
				pCoeffTable->wFilterCoeff[14] = FloatToS3_4(pSourceTable->FilterCoeff_0_14);
				pCoeffTable->wFilterCoeff[15] = FloatToS3_4(pSourceTable->FilterCoeff_0_15);
			}
		}

		for (int i = CM_NUM_CONVOLVE_ROWS_SKL; i < CM_NUM_CONVOLVE_ROWS_SKL * 2; i++)
		{
			MHW_SAMPLER_CONVOLVE_COEFF_TABLE *pCoeffTable = &(pSamplerEntry->Convolve.CoeffTable[i]);
			CM_HAL_CONVOLVE_COEFF_TABLE      *pSourceTable = &(pParam->sampler8x8State.convolveState.table[i - CM_NUM_CONVOLVE_ROWS_SKL]);

			if (pSamplerEntry->Convolve.ui8SizeOfTheCoefficient == 1)
			{
				pCoeffTable->wFilterCoeff[0] = FloatToS3_12(pSourceTable->FilterCoeff_0_16);
				pCoeffTable->wFilterCoeff[1] = FloatToS3_12(pSourceTable->FilterCoeff_0_17);
				pCoeffTable->wFilterCoeff[2] = FloatToS3_12(pSourceTable->FilterCoeff_0_18);
				pCoeffTable->wFilterCoeff[3] = FloatToS3_12(pSourceTable->FilterCoeff_0_19);
				pCoeffTable->wFilterCoeff[4] = FloatToS3_12(pSourceTable->FilterCoeff_0_20);
				pCoeffTable->wFilterCoeff[5] = FloatToS3_12(pSourceTable->FilterCoeff_0_21);
				pCoeffTable->wFilterCoeff[6] = FloatToS3_12(pSourceTable->FilterCoeff_0_22);
				pCoeffTable->wFilterCoeff[7] = FloatToS3_12(pSourceTable->FilterCoeff_0_23);
				pCoeffTable->wFilterCoeff[8] = FloatToS3_12(pSourceTable->FilterCoeff_0_24);
				pCoeffTable->wFilterCoeff[9] = FloatToS3_12(pSourceTable->FilterCoeff_0_25);
				pCoeffTable->wFilterCoeff[10] = FloatToS3_12(pSourceTable->FilterCoeff_0_26);
				pCoeffTable->wFilterCoeff[11] = FloatToS3_12(pSourceTable->FilterCoeff_0_27);
				pCoeffTable->wFilterCoeff[12] = FloatToS3_12(pSourceTable->FilterCoeff_0_28);
				pCoeffTable->wFilterCoeff[13] = FloatToS3_12(pSourceTable->FilterCoeff_0_29);
				pCoeffTable->wFilterCoeff[14] = FloatToS3_12(pSourceTable->FilterCoeff_0_30);
				pCoeffTable->wFilterCoeff[15] = FloatToS3_12(pSourceTable->FilterCoeff_0_31);
			}
			else
			{
				pCoeffTable->wFilterCoeff[0] = FloatToS3_4(pSourceTable->FilterCoeff_0_16);
				pCoeffTable->wFilterCoeff[1] = FloatToS3_4(pSourceTable->FilterCoeff_0_17);
				pCoeffTable->wFilterCoeff[2] = FloatToS3_4(pSourceTable->FilterCoeff_0_18);
				pCoeffTable->wFilterCoeff[3] = FloatToS3_4(pSourceTable->FilterCoeff_0_19);
				pCoeffTable->wFilterCoeff[4] = FloatToS3_4(pSourceTable->FilterCoeff_0_20);
				pCoeffTable->wFilterCoeff[5] = FloatToS3_4(pSourceTable->FilterCoeff_0_21);
				pCoeffTable->wFilterCoeff[6] = FloatToS3_4(pSourceTable->FilterCoeff_0_22);
				pCoeffTable->wFilterCoeff[7] = FloatToS3_4(pSourceTable->FilterCoeff_0_23);
				pCoeffTable->wFilterCoeff[8] = FloatToS3_4(pSourceTable->FilterCoeff_0_24);
				pCoeffTable->wFilterCoeff[9] = FloatToS3_4(pSourceTable->FilterCoeff_0_25);
				pCoeffTable->wFilterCoeff[10] = FloatToS3_4(pSourceTable->FilterCoeff_0_26);
				pCoeffTable->wFilterCoeff[11] = FloatToS3_4(pSourceTable->FilterCoeff_0_27);
				pCoeffTable->wFilterCoeff[12] = FloatToS3_4(pSourceTable->FilterCoeff_0_28);
				pCoeffTable->wFilterCoeff[13] = FloatToS3_4(pSourceTable->FilterCoeff_0_29);
				pCoeffTable->wFilterCoeff[14] = FloatToS3_4(pSourceTable->FilterCoeff_0_30);
				pCoeffTable->wFilterCoeff[15] = FloatToS3_4(pSourceTable->FilterCoeff_0_31);
			}
		}
	}

finish:
	return hr;
}

MOS_STATUS CM_HAL_G10_X::SetupHwDebugControl(    
    PRENDERHAL_INTERFACE   pRenderHal,
    PMOS_COMMAND_BUFFER    pCmdBuffer)
{
    MOS_STATUS  eStatus = MOS_STATUS_SUCCESS;
    MHW_MI_LOAD_REGISTER_IMM_PARAMS LoadRegImm;

    //---------------------------------------
    CM_MOS_CHK_NULL_RETURN( pRenderHal );
    CM_MOS_CHK_NULL_RETURN( pRenderHal->pMhwMiInterface );
    CM_MOS_CHK_NULL_RETURN( pCmdBuffer );
    //---------------------------------------

    MOS_ZeroMemory( &LoadRegImm, sizeof( MHW_MI_LOAD_REGISTER_IMM_PARAMS ) );

    // CS_DEBUG_MODE2, global debug enable
    LoadRegImm.dwRegister = CS_DEBUG_MODE2;
    LoadRegImm.dwData = ( CS_DEBUG_MODE2_GLOBAL_DEBUG << 16 ) | CS_DEBUG_MODE2_GLOBAL_DEBUG;
    CM_MOS_CHK_STATUS_RETURN( pRenderHal->pMhwMiInterface->AddMiLoadRegisterImmCmd( pCmdBuffer, &LoadRegImm ) );

    // TD_CTL, force thread breakpoint enable
    // Also enable external exception, because the source-level debugger has to
    // be able to interrupt runing EU threads.
    LoadRegImm.dwRegister = TD_CTL;
    LoadRegImm.dwData = TD_CTL_FORCE_THREAD_BKPT_ENABLE | TD_CTL_FORCE_EXT_EXCEPTION_ENABLE;
    CM_MOS_CHK_STATUS_RETURN( pRenderHal->pMhwMiInterface->AddMiLoadRegisterImmCmd( pCmdBuffer, &LoadRegImm ) );

    return eStatus;
}

MOS_STATUS CM_HAL_G10_X::SetMediaWalkerParams(
    CM_WALKING_PARAMETERS          engineeringParams,
    PCM_HAL_WALKER_PARAMS          pWalkerParams)
{
    mhw_render_g10_X::MEDIA_OBJECT_WALKER_CMD MWCmd;
    MWCmd.DW5.Value = engineeringParams.Value[0];
    pWalkerParams->scoreboardMask = MWCmd.DW5.ScoreboardMask;

    MWCmd.DW6.Value = engineeringParams.Value[1];
    pWalkerParams->colorCountMinusOne = MWCmd.DW6.ColorCountMinusOne;
    pWalkerParams->midLoopUnitX = MWCmd.DW6.MidLoopUnitX;
    pWalkerParams->midLoopUnitY = MWCmd.DW6.LocalMidLoopUnitY;
    pWalkerParams->middleLoopExtraSteps = MWCmd.DW6.MiddleLoopExtraSteps;

    MWCmd.DW7.Value = engineeringParams.Value[2];
    pWalkerParams->localLoopExecCount = MWCmd.DW7.LocalLoopExecCount;
    pWalkerParams->globalLoopExecCount = MWCmd.DW7.GlobalLoopExecCount;

    MWCmd.DW8.Value = engineeringParams.Value[3];
    pWalkerParams->blockResolution.x = MWCmd.DW8.BlockResolutionX;
    pWalkerParams->blockResolution.y = MWCmd.DW8.BlockResolutionY;

    MWCmd.DW9.Value = engineeringParams.Value[4];
    pWalkerParams->localStart.x = MWCmd.DW9.LocalStartX;
    pWalkerParams->localStart.y = MWCmd.DW9.LocalStartY;

    MWCmd.DW11.Value = engineeringParams.Value[6];
    pWalkerParams->localOutLoopStride.x = MWCmd.DW11.LocalOuterLoopStrideX;
    pWalkerParams->localOutLoopStride.y = MWCmd.DW11.LocalOuterLoopStrideY;

    MWCmd.DW12.Value = engineeringParams.Value[7];
    pWalkerParams->localInnerLoopUnit.x = MWCmd.DW12.LocalInnerLoopUnitX;
    pWalkerParams->localInnerLoopUnit.y = MWCmd.DW12.LocalInnerLoopUnitY;

    MWCmd.DW13.Value = engineeringParams.Value[8];
    pWalkerParams->globalResolution.x = MWCmd.DW13.GlobalResolutionX;
    pWalkerParams->globalResolution.y = MWCmd.DW13.GlobalResolutionY;

    MWCmd.DW14.Value = engineeringParams.Value[9];
    pWalkerParams->globalStart.x = MWCmd.DW14.GlobalStartX;
    pWalkerParams->globalStart.y = MWCmd.DW14.GlobalStartY;

    MWCmd.DW15.Value = engineeringParams.Value[10];
    pWalkerParams->globalOutlerLoopStride.x = MWCmd.DW15.GlobalOuterLoopStrideX;
    pWalkerParams->globalOutlerLoopStride.y = MWCmd.DW15.GlobalOuterLoopStrideY;

    MWCmd.DW16.Value = engineeringParams.Value[11];
    pWalkerParams->globalInnerLoopUnit.x = MWCmd.DW16.GlobalInnerLoopUnitX;
    pWalkerParams->globalInnerLoopUnit.y = MWCmd.DW16.GlobalInnerLoopUnitY;

    pWalkerParams->localEnd.x = 0;
    pWalkerParams->localEnd.y = 0;

    return MOS_STATUS_SUCCESS;
}

MOS_STATUS CM_HAL_G10_X::GetCopyKernelIsa(void  *&pIsa, uint32_t &IsaSize)
{
    pIsa = (void *)pGPUCopy_kernel_isa_gen10;
    IsaSize = iGPUCopy_kernel_isa_size_gen10;
    
    return MOS_STATUS_SUCCESS;
}

MOS_STATUS CM_HAL_G10_X::GetInitKernelIsa(void  *&pIsa, uint32_t &IsaSize)
{
    pIsa = (void *)pGPUInit_kernel_isa_Gen10;
    IsaSize = iGPUInit_kernel_isa_size_Gen10;

    return MOS_STATUS_SUCCESS;
}

uint32_t CM_HAL_G10_X::GetMediaWalkerMaxThreadWidth()
{
    return CM_MAX_THREADSPACE_WIDTH_SKLUP_FOR_MW;
}

uint32_t CM_HAL_G10_X::GetMediaWalkerMaxThreadHeight()
{
    return CM_MAX_THREADSPACE_HEIGHT_SKLUP_FOR_MW;
}

MOS_STATUS CM_HAL_G10_X::GetHwSurfaceBTIInfo(
                      PCM_SURFACE_BTI_INFO pBTIinfo)
{
    if (pBTIinfo == nullptr)
    {
        return MOS_STATUS_NULL_POINTER;
    }

    pBTIinfo->normalSurfaceStart      =  CM_GLOBAL_SURFACE_INDEX_START_GEN9_PLUS + \
                        CM_GLOBAL_SURFACE_NUMBER + CM_GTPIN_SURFACE_NUMBER ;
    pBTIinfo->normalSurfaceEnd        =  GT_RESERVED_INDEX_START_GEN9_PLUS - 1;
    pBTIinfo->reservedSurfaceStart    =  CM_GLOBAL_SURFACE_INDEX_START_GEN9_PLUS;
    pBTIinfo->reservedSurfaceEnd      =  CM_GLOBAL_SURFACE_NUMBER + CM_GTPIN_SURFACE_NUMBER;

    return MOS_STATUS_SUCCESS;
}

MOS_STATUS CM_HAL_G10_X::SetSuggestedL3Conf(
            L3_SUGGEST_CONFIG L3Conf)
{
    if (L3Conf >= sizeof(CNL_L3_PLANE)/sizeof(L3ConfigRegisterValues))
    {
        return MOS_STATUS_INVALID_PARAMETER;
    }
    return HalCm_SetL3Cache((L3ConfigRegisterValues *)&CNL_L3_PLANE[L3Conf],
                                     &m_cmState->l3Settings);
}

MOS_STATUS CM_HAL_G10_X::GetGenStepInfo(char*& stepinfostr)
{
    const char *CmSteppingInfo_CNL[] = {"A0", "B0", "C0"};

    uint32_t genStepId = m_cmState->platform.usRevId;

    uint32_t tablesize = sizeof(CmSteppingInfo_CNL) / sizeof(char *);

    if (genStepId < tablesize)
    {
        stepinfostr = (char *)CmSteppingInfo_CNL[genStepId];
    }
    else
    {
        stepinfostr = nullptr;
    }

    return MOS_STATUS_SUCCESS;
}

int32_t CM_HAL_G10_X::ColorCountSanityCheck(uint32_t colorCount)
{
    if (colorCount == CM_INVALID_COLOR_COUNT || colorCount > CM_THREADSPACE_MAX_COLOR_COUNT)
    {
        CM_ASSERTMESSAGE("Error: Invalid color count.");
        return CM_INVALID_ARG_VALUE;
    }
    return CM_SUCCESS;
}

bool CM_HAL_G10_X::MemoryObjectCtrlPolicyCheck(uint32_t memCtrl)
{
	if (memCtrl > MEMORY_OBJECT_CONTROL_SKL_NO_CACHE)
	{
		return false;
	}

	return true;
}

int32_t CM_HAL_G10_X::GetConvSamplerIndex(
    PMHW_SAMPLER_STATE_PARAM  pSamplerParam,
    char                     *pSamplerIndexTable,
    int32_t                   nSamp8X8Num,
    int32_t                   nSampConvNum)
{
    int32_t iSamplerIndex = 0;
    
    if ((pSamplerParam->Convolve.ui8ConvolveType == CM_CONVOLVE_SKL_TYPE_2D) &&
        (pSamplerParam->Convolve.skl_mode))
    {
        // 2D convolve & SKL+
        iSamplerIndex = 1 + nSampConvNum + nSamp8X8Num;
    }
    else if (pSamplerParam->Convolve.ui8ConvolveType == CM_CONVOLVE_SKL_TYPE_1D)
    {
        // 1D convolve & SKL+
        iSamplerIndex = nSampConvNum;
    }
    else
    {
        // 1P convolve SKL+
        iSamplerIndex = 1 + (nSamp8X8Num + nSampConvNum) * 2;
        while (pSamplerIndexTable[iSamplerIndex] != CM_INVALID_INDEX)
        {
            iSamplerIndex += 2;
        }

    }
    return iSamplerIndex;
}

MOS_STATUS CM_HAL_G10_X::SetL3CacheConfig(
            const L3ConfigRegisterValues *values_ptr, 
            PCmHalL3Settings cmhal_l3_cache_ptr)
{
    return HalCm_SetL3Cache( values_ptr, cmhal_l3_cache_ptr );
}

MOS_STATUS CM_HAL_G10_X::GetSamplerParamInfoForSamplerType(
            PMHW_SAMPLER_STATE_PARAM sampler_param_ptr,
            SamplerParam  &sampler_param)
{
    const unsigned int sampler_element_size[MAX_ELEMENT_TYPE_COUNT] = { 16, 32, 64, 128, 1024, 2048 };

    // gets element_type
    switch (sampler_param_ptr->SamplerType)
    {
    case MHW_SAMPLER_TYPE_3D:
        sampler_param.elementType = MHW_Sampler1Element;
        break;
    case MHW_SAMPLER_TYPE_MISC:
        sampler_param.elementType = MHW_Sampler2Elements;
        break;
    case MHW_SAMPLER_TYPE_CONV:
        if ((!sampler_param_ptr->Convolve.skl_mode &&
            sampler_param_ptr->Convolve.ui8ConvolveType == CM_CONVOLVE_SKL_TYPE_2D)
            || sampler_param_ptr->Convolve.ui8ConvolveType == CM_CONVOLVE_SKL_TYPE_1P)
        {
            sampler_param.elementType = MHW_Sampler64Elements;
        }
        else if (sampler_param_ptr->Convolve.ui8ConvolveType == CM_CONVOLVE_SKL_TYPE_1D)
        {
            sampler_param.elementType = MHW_Sampler8Elements;
        }
        else
        {
            sampler_param.elementType = MHW_Sampler128Elements;
        }
        break;
    case MHW_SAMPLER_TYPE_AVS:
        sampler_param.elementType = MHW_Sampler128Elements;
        break;
    default:
        break;
    }

    // bti_stepping for BDW mode convolve or 1P convolve is 2, other cases are 1.
    if ((sampler_param_ptr->SamplerType == MHW_SAMPLER_TYPE_CONV) && ((!sampler_param_ptr->Convolve.skl_mode &&
        sampler_param_ptr->Convolve.ui8ConvolveType == CM_CONVOLVE_SKL_TYPE_2D)
        || sampler_param_ptr->Convolve.ui8ConvolveType == CM_CONVOLVE_SKL_TYPE_1P))
    {
        sampler_param.btiStepping = 2;
    }
    else
    {
        sampler_param.btiStepping = 1;
    }

    // gets multiplier
    sampler_param.btiMultiplier = sampler_element_size[sampler_param.elementType] / sampler_param.btiStepping;

    // gets size
    sampler_param.size = sampler_element_size[sampler_param.elementType];

    // Temporary solution for conv because MHW use 2048 bytes for all of the convolve samplers.
    // size should always be equal to bti_stepping * bti_multiplier except for this one.
    if (sampler_param_ptr->SamplerType == MHW_SAMPLER_TYPE_CONV)
    {
        sampler_param.size = 2048;
    }

    return MOS_STATUS_SUCCESS;
}

MOS_STATUS CM_HAL_G10_X::GetExpectedGtSystemConfig(
    PCM_EXPECTED_GT_SYSTEM_INFO pExpectedConfig)
{
    if (m_genGT == PLATFORM_INTEL_GT1)
    {
        pExpectedConfig->numSlices    = CNL_GT1_4X8_MAX_NUM_SLICES;
        pExpectedConfig->numSubSlices = CNL_GT1_4X8_MAX_NUM_SUBSLICES;
    }
    else if (m_genGT == PLATFORM_INTEL_GT2)
    {
        pExpectedConfig->numSlices    = CNL_GT2_7X8_MAX_NUM_SLICES;
        pExpectedConfig->numSubSlices = CNL_GT2_7X8_MAX_NUM_SUBSLICES;
    }
    else if (m_genGT == PLATFORM_INTEL_GT3)
    {
        pExpectedConfig->numSlices    = CNL_GT3_9X8_MAX_NUM_SLICES;
        pExpectedConfig->numSubSlices = CNL_GT3_9_8MAX_NUM_SUBSLICES;
    }
    else
    {
        pExpectedConfig->numSlices    = 0;
        pExpectedConfig->numSubSlices = 0;
    }

    return MOS_STATUS_SUCCESS;
}

