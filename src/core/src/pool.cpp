// 多线程压缩：固定数量的工作线程从队列取文件，逐个调用 iis_compress_file。
// 同一路径在排队或处理中时重复提交被忽略，避免两个线程同时处理同一文件。
#include "commit.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

struct iis_pool {
    struct Job {
        std::string path, rel;
    };

    iis_file_options opt{};
    iis_pool_callback cb = nullptr;
    void* user = nullptr;

    std::mutex mu;
    std::condition_variable work_cv, idle_cv;
    std::deque<Job> queue;
    std::unordered_set<std::string> active;  // 排队中 + 处理中
    size_t running = 0;
    bool paused = false, stopping = false;
    std::vector<std::thread> threads;

    void worker() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lk(mu);
                work_cv.wait(lk, [&] { return stopping || (!paused && !queue.empty()); });
                if (stopping) return;
                job = std::move(queue.front());
                queue.pop_front();
                ++running;
            }
            iis_file_options o = opt;
            o.compress.rel_path_utf8 = job.rel.empty() ? nullptr : job.rel.c_str();
            iis_file_result r{};
            int st;
            try {
                st = iis::commit::compress_file(job.path, o, r);
            } catch (const std::bad_alloc&) {
                st = iis::fail(IIS_E_NO_MEMORY, "内存不足");
            } catch (const std::exception& e) {
                st = iis::fail(IIS_E_INTERNAL, std::string("内部错误：") + e.what());
            } catch (...) {
                st = iis::fail(IIS_E_INTERNAL, "内部错误：未知异常");
            }
            if (cb) cb(user, job.path.c_str(), st, &r, st == IIS_OK ? "" : iis_last_error());
            {
                std::lock_guard<std::mutex> lk(mu);
                active.erase(job.path);
                --running;
                if (queue.empty() && running == 0) idle_cv.notify_all();
            }
        }
    }
};

using namespace iis;

extern "C" {

int iis_pool_create(int threads, const iis_file_options* opt, iis_pool_callback cb, void* user, iis_pool** out) {
    IIS_GUARD_BEGIN
    if (!out) return fail(IIS_E_INVALID_ARG, "参数为空");
    *out = nullptr;
    if (threads < 1 || threads > 256) return fail(IIS_E_INVALID_ARG, "线程数须为 1~256");
    auto p = std::make_unique<iis_pool>();
    if (opt) p->opt = *opt;
    p->opt.compress.rel_path_utf8 = nullptr;
    p->cb = cb;
    p->user = user;
    for (int i = 0; i < threads; ++i) p->threads.emplace_back([q = p.get()] { q->worker(); });
    *out = p.release();
    return IIS_OK;
    IIS_GUARD_END
}

int iis_pool_submit(iis_pool* p, const char* path_utf8, const char* rel_path_utf8) {
    IIS_GUARD_BEGIN
    if (!p || !path_utf8 || !*path_utf8) return fail(IIS_E_INVALID_ARG, "参数为空");
    {
        std::lock_guard<std::mutex> lk(p->mu);
        if (!p->active.insert(path_utf8).second) return IIS_OK;
        p->queue.push_back({path_utf8, rel_path_utf8 ? rel_path_utf8 : ""});
    }
    p->work_cv.notify_one();
    return IIS_OK;
    IIS_GUARD_END
}

void iis_pool_pause(iis_pool* p, int paused) {
    if (!p) return;
    {
        std::lock_guard<std::mutex> lk(p->mu);
        p->paused = paused != 0;
    }
    p->work_cv.notify_all();
}

size_t iis_pool_pending(iis_pool* p) {
    if (!p) return 0;
    std::lock_guard<std::mutex> lk(p->mu);
    return p->queue.size() + p->running;
}

void iis_pool_wait(iis_pool* p) {
    if (!p) return;
    std::unique_lock<std::mutex> lk(p->mu);
    p->idle_cv.wait(lk, [&] { return p->queue.empty() && p->running == 0; });
}

void iis_pool_destroy(iis_pool* p) {
    if (!p) return;
    {
        std::lock_guard<std::mutex> lk(p->mu);
        p->stopping = true;
        p->queue.clear();
    }
    p->work_cv.notify_all();
    for (auto& t : p->threads) t.join();
    delete p;
}

}  // extern "C"
