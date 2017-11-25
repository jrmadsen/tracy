#ifndef __TRACYOPENGL_HPP__
#define __TRACYOPENGL_HPP__

// Include this file after you include OpenGL 3.2 headers.

#if !defined TRACY_ENABLE || defined __APPLE__

#define TracyGpuContext
#define TracyGpuZone(x)
#define TracyGpuZoneC(x,y)
#define TracyGpuCollect

#else

#include <atomic>

#include "Tracy.hpp"
#include "client/TracyProfiler.hpp"
#include "common/TracyAlloc.hpp"

#define TracyGpuContext tracy::s_gpuCtx.ptr = (tracy::GpuCtx*)tracy::tracy_malloc( sizeof( tracy::GpuCtx ) ); new(tracy::s_gpuCtx.ptr) tracy::GpuCtx;
#define TracyGpuZone( name ) static const tracy::SourceLocation __tracy_gpu_source_location { name, __FUNCTION__,  __FILE__, (uint32_t)__LINE__, 0 }; tracy::GpuCtxScope ___tracy_gpu_zone( &__tracy_gpu_source_location );
#define TracyGpuZoneC( name, color ) static const tracy::SourceLocation __tracy_gpu_source_location { name, __FUNCTION__,  __FILE__, (uint32_t)__LINE__, color }; tracy::GpuCtxScope ___tracy_gpu_zone( &__tracy_gpu_source_location );
#define TracyGpuCollect tracy::s_gpuCtx.ptr->Collect();

namespace tracy
{

extern std::atomic<uint16_t> s_gpuCtxCounter;

class GpuCtx
{
    friend class GpuCtxScope;

    enum { QueryCount = 64 * 1024 };

public:
    GpuCtx()
        : m_context( s_gpuCtxCounter.fetch_add( 1, std::memory_order_relaxed ) )
        , m_head( 0 )
        , m_tail( 0 )
    {
        glGenQueries( QueryCount, m_query );

        int64_t tgpu;
        glGetInteger64v( GL_TIMESTAMP, &tgpu );
        int64_t tcpu = Profiler::GetTime();

        GLint bits;
        glGetQueryiv( GL_TIMESTAMP, GL_QUERY_COUNTER_BITS, &bits );

        Magic magic;
        auto& token = s_token.ptr;
        auto& tail = token->get_tail_index();
        auto item = token->enqueue_begin<moodycamel::CanAlloc>( magic );
        item->hdr.type = QueueType::GpuNewContext;
        item->gpuNewContext.cpuTime = tcpu;
        item->gpuNewContext.gpuTime = tgpu;
        item->gpuNewContext.thread = GetThreadHandle();
        item->gpuNewContext.context = m_context;
        item->gpuNewContext.accuracyBits = bits;
        tail.store( magic + 1, std::memory_order_release );
    }

    void Collect()
    {
        ZoneScopedC( 0x881111 );

        auto start = m_tail;
        auto end = m_head + QueryCount;
        auto cnt = ( end - start ) % QueryCount;
        while( cnt > 1 )
        {
            auto mid = start + cnt / 2;
            GLint available;
            glGetQueryObjectiv( m_query[mid % QueryCount], GL_QUERY_RESULT_AVAILABLE, &available );
            if( available )
            {
                start = mid;
            }
            else
            {
                end = mid;
            }
            cnt = ( end - start ) % QueryCount;
        }

        start %= QueryCount;

        while( m_tail != start )
        {
            uint64_t time;
            glGetQueryObjectui64v( m_query[m_tail], GL_QUERY_RESULT, &time );

            Magic magic;
            auto& token = s_token.ptr;
            auto& tail = token->get_tail_index();
            auto item = token->enqueue_begin<moodycamel::CanAlloc>( magic );
            item->hdr.type = QueueType::GpuTime;
            item->gpuTime.gpuTime = (int64_t)time;
            item->gpuTime.context = m_context;
            tail.store( magic + 1, std::memory_order_release );
            m_tail = ( m_tail + 1 ) % QueryCount;
        }
    }

private:
    tracy_force_inline unsigned int NextQueryId()
    {
        const auto id = m_head;
        m_head = ( m_head + 1 ) % QueryCount;
        assert( m_head != m_tail );
        return m_query[id];
    }

    tracy_force_inline uint16_t GetId() const
    {
        return m_context;
    }

    unsigned int m_query[QueryCount];
    uint16_t m_context;

    unsigned int m_head;
    unsigned int m_tail;
};

extern thread_local GpuCtxWrapper s_gpuCtx;

class GpuCtxScope
{
public:
    tracy_force_inline GpuCtxScope( const SourceLocation* srcloc )
    {
        glQueryCounter( s_gpuCtx.ptr->NextQueryId(), GL_TIMESTAMP );

        Magic magic;
        auto& token = s_token.ptr;
        auto& tail = token->get_tail_index();
        auto item = token->enqueue_begin<moodycamel::CanAlloc>( magic );
        item->hdr.type = QueueType::GpuZoneBegin;
        item->gpuZoneBegin.cpuTime = Profiler::GetTime();
        item->gpuZoneBegin.srcloc = (uint64_t)srcloc;
        item->gpuZoneBegin.context = s_gpuCtx.ptr->GetId();
        tail.store( magic + 1, std::memory_order_release );
    }

    tracy_force_inline ~GpuCtxScope()
    {
        glQueryCounter( s_gpuCtx.ptr->NextQueryId(), GL_TIMESTAMP );

        Magic magic;
        auto& token = s_token.ptr;
        auto& tail = token->get_tail_index();
        auto item = token->enqueue_begin<moodycamel::CanAlloc>( magic );
        item->hdr.type = QueueType::GpuZoneEnd;
        item->gpuZoneEnd.cpuTime = Profiler::GetTime();
        item->gpuZoneEnd.context = s_gpuCtx.ptr->GetId();
        tail.store( magic + 1, std::memory_order_release );
    }
};

}

#endif

#endif
