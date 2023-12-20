#include "thread_pool.hpp"
using namespace tido::types;

ThreadPool::~ThreadPool()
{
}

void ThreadPool::worker(std::shared_ptr<ThreadPool::SharedData> shared_data, u32 thread_index)
{
    TaskChunk current_chunk = {};
    while (true)
    {
        std::unique_lock lock{shared_data->threadpool_mutex};
        shared_data->work_available.wait(
            lock, [&] { return !shared_data->high_priority_tasks.empty() || !shared_data->low_priority_tasks.empty(); });

        bool const high_priority_work_available = !shared_data->high_priority_tasks.empty();
        auto & selected_queue = high_priority_work_available ? shared_data->high_priority_tasks : shared_data->low_priority_tasks;
        current_chunk = std::move(selected_queue.front());
        selected_queue.pop_front();
        current_chunk.task->started += 1;
        lock.unlock();

        // Received invalid chunk code in the chunk index which exits this thread
        if (current_chunk.chunk_index == EXIT_CHUNK_CODE) { return; }
        current_chunk.task->callback(current_chunk.chunk_index, thread_index);

        lock.lock();
        current_chunk.task->not_finished -= 1;
        // Working on last chunk of a task, notify in case there is a thread waiting for this task to be done
        if (current_chunk.task->not_finished == 0) { shared_data->work_done.notify_all(); }
    }
}

ThreadPool::ThreadPool(std::optional<u32> thread_count)
{
    u32 const real_thread_count = thread_count.value_or(std::thread::hardware_concurrency());
    shared_data = std::make_shared<SharedData>();
    for (u32 thread_index = 0; thread_index < real_thread_count; thread_index++)
    {
        worker_threads.push_back({
            std::thread([=, this]() { ThreadPool::worker(shared_data, thread_index); }),
        });
    }
}

void ThreadPool::blocking_dispatch(std::shared_ptr<Task> task, TaskPriority priority)
{
    // Don't need mutex here as no thread is working on this task yet
    task->not_finished = task->chunk_count;
    std::unique_lock lock{shared_data->threadpool_mutex, std::defer_lock};
    auto & selected_queue = priority == TaskPriority::HIGH ? shared_data->high_priority_tasks : shared_data->low_priority_tasks;

    lock.lock();
    // chunk_index 0 will be worked on by this thread
    for (u32 chunk_index = 1; chunk_index < task->chunk_count; chunk_index++)
    {
        selected_queue.push_back({task, chunk_index});
    }
    lock.unlock();

    shared_data->work_available.notify_all();

    // Contribute to finishing this task from this thread
    u32 current_chunk_index = 0;
    bool worked_on_last_chunk = false;
    while (current_chunk_index != NO_MORE_CHUNKS_CODE)
    {
        lock.lock();
        task->started += 1;
        lock.unlock();

        task->callback(current_chunk_index, EXTERNAL_THREAD_INDEX);

        lock.lock();
        task->not_finished -= 1;
        bool more_chunks_in_queue = (task->started != task->chunk_count);
        if (more_chunks_in_queue)
        {
            current_chunk_index = selected_queue.front().chunk_index;
            selected_queue.pop_front();
        }
        else { current_chunk_index = NO_MORE_CHUNKS_CODE; }

        worked_on_last_chunk = (task->not_finished == 0);
        lock.unlock();
    }

    if (!worked_on_last_chunk)
    {
        // This thread was not the last one working on this task, therefore we wait here to be notified once
        // the last worker thread processing this task is done
        lock.lock();
        shared_data->work_done.wait(lock, [&] { return task->not_finished == 0; });
    }
}

void ThreadPool::async_dispatch(std::shared_ptr<Task> task, TaskPriority priority)
{
    auto & selected_queue = priority == TaskPriority::HIGH ? shared_data->high_priority_tasks : shared_data->low_priority_tasks;
    {
        std::lock_guard lock(shared_data->threadpool_mutex);
        for (u32 chunk_index = 0; chunk_index < task->chunk_count; chunk_index++)
        {
            selected_queue.push_back({task, chunk_index});
        }
    }
    shared_data->work_available.notify_all();
}