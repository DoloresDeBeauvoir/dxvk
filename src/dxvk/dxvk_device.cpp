#include "dxvk_device.h"
#include "dxvk_instance.h"

namespace dxvk {
  
  DxvkDevice::DxvkDevice(
          std::string               clientApi,
    const Rc<DxvkAdapter>&          adapter,
    const Rc<vk::DeviceFn>&         vkd,
    const DxvkDeviceExtensions&     extensions,
    const DxvkDeviceFeatures&       features)
  : m_clientApi         (clientApi),
    m_options           (adapter->instance()->options()),
    m_adapter           (adapter),
    m_vkd               (vkd),
    m_extensions        (extensions),
    m_features          (features),
    m_properties        (adapter->deviceProperties()),
    m_memory            (new DxvkMemoryAllocator    (this)),
    m_renderPassPool    (new DxvkRenderPassPool     (vkd)),
    m_pipelineManager   (new DxvkPipelineManager    (this, m_renderPassPool.ptr())),
    m_gpuEventPool      (new DxvkGpuEventPool       (vkd)),
    m_gpuQueryPool      (new DxvkGpuQueryPool       (this)),
    m_metaClearObjects  (new DxvkMetaClearObjects   (vkd)),
    m_metaCopyObjects   (new DxvkMetaCopyObjects    (vkd)),
    m_metaMipGenObjects (new DxvkMetaMipGenObjects  (vkd)),
    m_metaPackObjects   (new DxvkMetaPackObjects    (vkd)),
    m_metaResolveObjects(new DxvkMetaResolveObjects (vkd)),
    m_unboundResources  (this),
    m_submissionQueue   (this) {
    m_graphicsQueue.queueFamily = m_adapter->graphicsQueueFamily();
    m_presentQueue.queueFamily  = m_adapter->presentQueueFamily();
    
    m_vkd->vkGetDeviceQueue(m_vkd->device(),
      m_graphicsQueue.queueFamily, 0,
      &m_graphicsQueue.queueHandle);
    
    m_vkd->vkGetDeviceQueue(m_vkd->device(),
      m_presentQueue.queueFamily, 0,
      &m_presentQueue.queueHandle);
  }
  
  
  DxvkDevice::~DxvkDevice() {
    // Wait for all pending Vulkan commands to be
    // executed before we destroy any resources.
    m_vkd->vkDeviceWaitIdle(m_vkd->device());
  }


  VkPipelineStageFlags DxvkDevice::getShaderPipelineStages() const {
    VkPipelineStageFlags result = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT
                                | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    
    if (m_features.core.features.geometryShader)
      result |= VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT;
    
    if (m_features.core.features.tessellationShader) {
      result |= VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT
             |  VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT;
    }

    return result;
  }


  DxvkDeviceOptions DxvkDevice::options() const {
    DxvkDeviceOptions options;
    options.maxNumDynamicUniformBuffers = m_properties.limits.maxDescriptorSetUniformBuffersDynamic;
    options.maxNumDynamicStorageBuffers = m_properties.limits.maxDescriptorSetStorageBuffersDynamic;
    return options;
  }
  
  
  Rc<DxvkStagingBuffer> DxvkDevice::allocStagingBuffer(VkDeviceSize size) {
    // In case we need a standard-size staging buffer, try
    // to recycle an old one that has been returned earlier
    if (size <= DefaultStagingBufferSize) {
      const Rc<DxvkStagingBuffer> buffer
        = m_recycledStagingBuffers.retrieveObject();
      
      if (buffer != nullptr)
        return buffer;
    }
    
    // Staging buffers only need to be able to handle transfer
    // operations, and they need to be in host-visible memory.
    DxvkBufferCreateInfo info;
    info.size   = size;
    info.usage  = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.stages = VK_PIPELINE_STAGE_TRANSFER_BIT
                | VK_PIPELINE_STAGE_HOST_BIT;
    info.access = VK_ACCESS_TRANSFER_READ_BIT
                | VK_ACCESS_HOST_WRITE_BIT;
    
    VkMemoryPropertyFlags memFlags
      = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
      | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    
    // Don't create buffers that are too small. A staging
    // buffer should be able to serve multiple uploads.
    if (info.size < DefaultStagingBufferSize)
      info.size = DefaultStagingBufferSize;
    
    return new DxvkStagingBuffer(this->createBuffer(info, memFlags));
  }
  
  
  void DxvkDevice::recycleStagingBuffer(const Rc<DxvkStagingBuffer>& buffer) {
    // Drop staging buffers that are bigger than the
    // standard ones to save memory, recycle the rest
    if (buffer->size() == DefaultStagingBufferSize) {
      m_recycledStagingBuffers.returnObject(buffer);
      buffer->reset();
    }
  }
  
  
  Rc<DxvkCommandList> DxvkDevice::createCommandList() {
    Rc<DxvkCommandList> cmdList = m_recycledCommandLists.retrieveObject();
    
    if (cmdList == nullptr) {
      cmdList = new DxvkCommandList(this,
        m_adapter->graphicsQueueFamily());
    }
    
    return cmdList;
  }


  Rc<DxvkDescriptorPool> DxvkDevice::createDescriptorPool() {
    Rc<DxvkDescriptorPool> pool = m_recycledDescriptorPools.retrieveObject();

    if (pool == nullptr)
      pool = new DxvkDescriptorPool(m_vkd);
    
    return pool;
  }
  
  
  Rc<DxvkContext> DxvkDevice::createContext() {
    return new DxvkContext(this,
      m_pipelineManager,
      m_gpuEventPool,
      m_gpuQueryPool,
      m_metaClearObjects,
      m_metaCopyObjects,
      m_metaMipGenObjects,
      m_metaPackObjects,
      m_metaResolveObjects);
  }


  Rc<DxvkGpuEvent> DxvkDevice::createGpuEvent() {
    return new DxvkGpuEvent(m_vkd);
  }


  Rc<DxvkGpuQuery> DxvkDevice::createGpuQuery(
          VkQueryType           type,
          VkQueryControlFlags   flags,
          uint32_t              index) {
    return new DxvkGpuQuery(m_vkd, type, flags, index);
  }
  
  
  Rc<DxvkFramebuffer> DxvkDevice::createFramebuffer(
    const DxvkRenderTargets& renderTargets) {
    const DxvkFramebufferSize defaultSize = {
      m_properties.limits.maxFramebufferWidth,
      m_properties.limits.maxFramebufferHeight,
      m_properties.limits.maxFramebufferLayers };
    
    auto renderPassFormat = DxvkFramebuffer::getRenderPassFormat(renderTargets);
    auto renderPassObject = m_renderPassPool->getRenderPass(renderPassFormat);
    
    return new DxvkFramebuffer(m_vkd,
      renderPassObject, renderTargets, defaultSize);
  }
  
  
  Rc<DxvkBuffer> DxvkDevice::createBuffer(
    const DxvkBufferCreateInfo& createInfo,
          VkMemoryPropertyFlags memoryType) {
    return new DxvkBuffer(this, createInfo, *m_memory, memoryType);
  }
  
  
  Rc<DxvkBufferView> DxvkDevice::createBufferView(
    const Rc<DxvkBuffer>&           buffer,
    const DxvkBufferViewCreateInfo& createInfo) {
    return new DxvkBufferView(m_vkd, buffer, createInfo);
  }
  
  
  Rc<DxvkImage> DxvkDevice::createImage(
    const DxvkImageCreateInfo&  createInfo,
          VkMemoryPropertyFlags memoryType) {
    return new DxvkImage(m_vkd, createInfo, *m_memory, memoryType);
  }
  
  
  Rc<DxvkImageView> DxvkDevice::createImageView(
    const Rc<DxvkImage>&            image,
    const DxvkImageViewCreateInfo&  createInfo) {
    return new DxvkImageView(m_vkd, image, createInfo);
  }
  
  
  Rc<DxvkSampler> DxvkDevice::createSampler(
    const DxvkSamplerCreateInfo&  createInfo) {
    return new DxvkSampler(m_vkd, createInfo);
  }
  
  
  Rc<DxvkShader> DxvkDevice::createShader(
          VkShaderStageFlagBits     stage,
          uint32_t                  slotCount,
    const DxvkResourceSlot*         slotInfos,
    const DxvkInterfaceSlots&       iface,
    const SpirvCodeBuffer&          code) {
    return new DxvkShader(stage,
      slotCount, slotInfos, iface, code,
      DxvkShaderOptions(),
      DxvkShaderConstData());
  }
  
  
  DxvkStatCounters DxvkDevice::getStatCounters() {
    DxvkMemoryStats mem = m_memory->getMemoryStats();
    DxvkPipelineCount pipe = m_pipelineManager->getPipelineCount();
    
    DxvkStatCounters result;
    result.setCtr(DxvkStatCounter::MemoryAllocated,   mem.memoryAllocated);
    result.setCtr(DxvkStatCounter::MemoryUsed,        mem.memoryUsed);
    result.setCtr(DxvkStatCounter::PipeCountGraphics, pipe.numGraphicsPipelines);
    result.setCtr(DxvkStatCounter::PipeCountCompute,  pipe.numComputePipelines);
    
    std::lock_guard<sync::Spinlock> lock(m_statLock);
    result.merge(m_statCounters);
    return result;
  }


  uint32_t DxvkDevice::getCurrentFrameId() const {
    return m_statCounters.getCtr(DxvkStatCounter::QueuePresentCount);
  }
  
  
  void DxvkDevice::initResources() {
    m_unboundResources.clearResources(this);
  }


  void DxvkDevice::registerShader(const Rc<DxvkShader>& shader) {
    m_pipelineManager->registerShader(shader);
  }
  
  
  VkResult DxvkDevice::presentImage(
    const Rc<vk::Presenter>&        presenter,
          VkSemaphore               semaphore) {
    std::lock_guard<std::mutex> queueLock(m_submissionLock);
    VkResult status = presenter->presentImage(semaphore);

    if (status != VK_SUCCESS)
      return status;
    
    std::lock_guard<sync::Spinlock> statLock(m_statLock);
    m_statCounters.addCtr(DxvkStatCounter::QueuePresentCount, 1);
    return status;
  }


  void DxvkDevice::submitCommandList(
    const Rc<DxvkCommandList>&      commandList,
          VkSemaphore               waitSync,
          VkSemaphore               wakeSync) {
    VkResult status;
    
    { // Queue submissions are not thread safe
      std::lock_guard<std::mutex> queueLock(m_submissionLock);
      std::lock_guard<sync::Spinlock> statLock(m_statLock);
      
      m_statCounters.merge(commandList->statCounters());
      m_statCounters.addCtr(DxvkStatCounter::QueueSubmitCount, 1);
      
      status = commandList->submit(
        m_graphicsQueue.queueHandle,
        waitSync, wakeSync);
    }
    
    if (status == VK_SUCCESS) {
      // Add this to the set of running submissions
      m_submissionQueue.submit(commandList);
    } else {
      Logger::err(str::format(
        "DxvkDevice: Command buffer submission failed: ",
        status));
    }
  }
  
  
  void DxvkDevice::waitForIdle() {
    if (m_vkd->vkDeviceWaitIdle(m_vkd->device()) != VK_SUCCESS)
      Logger::err("DxvkDevice: waitForIdle: Operation failed");
  }
  
  
  void DxvkDevice::recycleCommandList(const Rc<DxvkCommandList>& cmdList) {
    m_recycledCommandLists.returnObject(cmdList);
  }
  

  void DxvkDevice::recycleDescriptorPool(const Rc<DxvkDescriptorPool>& pool) {
    m_recycledDescriptorPools.returnObject(pool);
  }
  
}
