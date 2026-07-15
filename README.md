🎨 JLib Engine: DirectX 12 Rendering Pipeline
The rendering architecture is a data-oriented, multi-threaded frontend built on DirectX 12. Instead of standard object-oriented immediate rendering (which forces the CPU to wait on the GPU), the engine leverages thread-local command storage buckets to parallelize command recording across the TaskScheduler.

🏎️ 1. Multi-Threaded Command Recording (The Ingestion Matrix)
To eliminate main-thread bottlenecks, command recording is decoupled from the actual graphics context submission.

[ Worker Thread 1 ] ──► Records to ──► Local Storage Bucket 1 ──┐
[ Worker Thread 2 ] ──► Records to ──► Local Storage Bucket 2 ──┼──► Submitted Single-Threaded
[ Worker Thread 3 ] ──► Records to ──► Local Storage Bucket 3 ──┘       via TaskDAG Main Nodes
Renderer::m_WorkerLocalStorage & m_Buckets: Each worker thread or task node records structural draw calls, state changes, and pipeline bindings into thread-isolated storage arrays. Because no two threads write to the same storage bucket, there is zero contention or mutex lock overhead during the heavy update loop.

The Main-Thread Affinity Lock: While recording commands can happen 24-cores wide, the final submission to the DX12 command queue handle is inherently single-threaded. This is why rendering dispatch relies on TaskDAG::CreateMainNode. These nodes wait for worker tasks to complete, then execute exclusively on the primary thread loop.

🔄 2. Resource Lifecycle & Descriptor Management
Modern rendering requires explicit control over how resources (textures, buffers) are mapped and tracked in silicon.

Slab and Transient Allocation: Just like the scheduler's tasks, dynamic data—such as per-frame constant buffers, particle instance data updates, and vertex array updates—are managed via transient memory segments. This guarantees perfect cache alignment and eliminates per-frame runtime allocations.

Descriptor Tables & Bindless Rendering: Rather than constantly rebinding slot-based textures (which stalls the pipeline), the engine relies on long, contiguous descriptor heaps. Shaders read descriptors directly out of unified arrays, ensuring maximum throughput when swapping materials or processing massive sprite batches.

🧱 3. The 3-Tier Class Division
The implementation files separate structural device setups from practical gameplay rendering:

🔹 1. RendererCore (The Hardware Layer)
Responsibility: Manages the low-level DX12 boilerplate: Factory creation, Adapter selection, the Command Queue, Swap Chain presentation, Fence synchronization, and the global Descriptor Heaps.

Rule: This layer is strictly foundational. It does not know about gameplay objects, sprites, or particles; it only manages the raw channel to the graphics hardware.

🔹 2. Renderer2D (The Batch Layer)
Responsibility: Handles high-performance 2D orthographic projection, primitive quad drawing, font typography matrix mapping, and text rendering.

Mechanism: Aggregates individual draw operations into massive unified vertex batches to keep the GPU pipeline saturated and minimize state changes.

🔹 3. Renderer (The Orchestrator)
Responsibility: Interfaces directly with the engine game loop. It flushes the worker local storage buckets, resolves frame synchronization fences, updates global game time variables, and drives the multi-buffered frame execution cycle.

🛡️ Critical Engine Rules for the Graphics Matrix
Affinity Compliance: Never call command queue submissions or swap chain presents inside a standard TaskDAG worker node. Doing so will breach the thread isolation model and trigger an immediate device removal or crash. Use CreateMainNode for all submissions.

Buffer Synchronization: When reading from shared instance arrays (like updating particle screen positions directly from the SoA physics vectors), ensure the preceding physics update graph nodes have completely settled before flushing the rendering buckets.

Descriptor Allocations: All custom game assets or dynamic texture updates must pass through the ResourceManager to guarantee their descriptor slots are securely registered ahead of the frame's recording pass.

there are currently two pipelines for sprite atlases and animations -- for individual pngs theres atlaspacker.exe included with the renderer and atlasanimation.h

theres also GridSpriteSheet and GridSpriteAnimation for existing packed/sparse tilesheets either works depending on your content type
