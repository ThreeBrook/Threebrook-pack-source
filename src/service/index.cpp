#include "index.hpp"

#include "formats.hpp"

#include <sqlite3.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <stdexcept>

namespace iis::svc {

namespace {

struct Stmt {
    sqlite3_stmt* s = nullptr;
    Stmt(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK)
            throw std::runtime_error(std::string("索引 SQL 有误：") + sqlite3_errmsg(db));
    }
    ~Stmt() { sqlite3_finalize(s); }
    Stmt& bind(int i, const std::string& v) {
        sqlite3_bind_text(s, i, v.data(), int(v.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Stmt& bind(int i, int64_t v) {
        sqlite3_bind_int64(s, i, v);
        return *this;
    }
    bool step() {
        const int rc = sqlite3_step(s);
        if (rc == SQLITE_ROW) return true;
        if (rc != SQLITE_DONE) throw std::runtime_error(std::string("索引读写失败：") + sqlite3_errmsg(sqlite3_db_handle(s)));
        return false;
    }
    std::string text(int i) {
        const auto* p = sqlite3_column_text(s, i);
        return p ? std::string(reinterpret_cast<const char*>(p), size_t(sqlite3_column_bytes(s, i))) : std::string();
    }
    int64_t i64(int i) { return sqlite3_column_int64(s, i); }
};

const char* kCols = "path, root, rel, station, product, batch, verdict, mtime, orig_size, disk_size, sha256, state, updated";

Row read_row(Stmt& q) {
    Row r;
    r.path = q.text(0);
    r.root = q.text(1);
    r.rel = q.text(2);
    r.station = q.text(3);
    r.product = q.text(4);
    r.batch = q.text(5);
    r.verdict = q.text(6);
    r.mtime_ms = q.i64(7);
    r.orig_size = uint64_t(q.i64(8));
    r.disk_size = uint64_t(q.i64(9));
    r.sha256 = q.text(10);
    r.state = int(q.i64(11));
    r.updated_ms = q.i64(12);
    return r;
}

}  // namespace

const char* state_name(int s) {
    switch (s) {
        case ST_PENDING: return "待压缩";
        case ST_COMPRESSED: return "已压缩";
        case ST_STORED: return "原样保存";
        case ST_DELETED: return "已删除";
        default: return "?";
    }
}

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string local_day(int64_t ms) {
    const std::time_t t = std::time_t(ms / 1000);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[16];
    std::strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
    return buf;
}

Index::~Index() {
    if (db_) sqlite3_close(db_);
}

void Index::exec(const char* sql) {
    char* msg = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
        const std::string m = msg ? msg : "";
        sqlite3_free(msg);
        throw std::runtime_error("索引 SQL 失败：" + m);
    }
}

bool Index::open(const std::string& file, std::string& err) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(file).parent_path(), ec);
    if (sqlite3_open_v2(file.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        err = "无法打开索引 " + file + "：" + (db_ ? sqlite3_errmsg(db_) : "");
        return false;
    }
    sqlite3_busy_timeout(db_, 10000);  // 命令行导出与服务同时访问
    try {
        exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;");
        exec(R"(CREATE TABLE IF NOT EXISTS files(
                  path TEXT PRIMARY KEY, root TEXT NOT NULL, rel TEXT NOT NULL,
                  station TEXT NOT NULL, product TEXT NOT NULL, batch TEXT NOT NULL, verdict TEXT NOT NULL,
                  mtime INTEGER NOT NULL, orig_size INTEGER NOT NULL, disk_size INTEGER NOT NULL,
                  sha256 TEXT NOT NULL, state INTEGER NOT NULL, updated INTEGER NOT NULL) WITHOUT ROWID;
                CREATE INDEX IF NOT EXISTS files_age ON files(root, state, mtime);
                CREATE INDEX IF NOT EXISTS files_time ON files(mtime);
                CREATE TABLE IF NOT EXISTS daily(day TEXT PRIMARY KEY, files INTEGER, bytes_in INTEGER,
                  bytes_out INTEGER, deleted INTEGER, deleted_bytes INTEGER);
                CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT);)");
        Stmt q(db_, "SELECT root, COUNT(*), SUM(disk_size), SUM(orig_size) FROM files WHERE state < 3 GROUP BY root");
        while (q.step()) totals_[q.text(0)] = {uint64_t(q.i64(1)), uint64_t(q.i64(2)), uint64_t(q.i64(3))};
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    return true;
}

bool Index::get(const std::string& path, Row& out) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    Stmt q(db_, (std::string("SELECT ") + kCols + " FROM files WHERE path = ?").c_str());
    q.bind(1, path);
    if (!q.step()) return false;
    out = read_row(q);
    return true;
}

void Index::contribute(const Row& r, int sign) {
    if (r.state >= ST_DELETED) return;
    Totals& t = totals_[r.root];
    if (sign > 0) {
        t.files += 1;
        t.used += r.disk_size;
        t.equiv += r.orig_size;
    } else {
        t.files -= std::min<uint64_t>(t.files, 1);
        t.used -= std::min(t.used, r.disk_size);
        t.equiv -= std::min(t.equiv, r.orig_size);
    }
}

void Index::upsert(Row r) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    Row old;
    if (get(r.path, old)) contribute(old, -1);
    r.updated_ms = now_ms();
    Stmt q(db_, (std::string("INSERT OR REPLACE INTO files(") + kCols + ") VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)").c_str());
    q.bind(1, r.path).bind(2, r.root).bind(3, r.rel).bind(4, r.station).bind(5, r.product).bind(6, r.batch);
    q.bind(7, r.verdict).bind(8, r.mtime_ms).bind(9, int64_t(r.orig_size)).bind(10, int64_t(r.disk_size));
    q.bind(11, r.sha256).bind(12, int64_t(r.state)).bind(13, r.updated_ms);
    q.step();
    contribute(r, +1);
}

void Index::scan_oldest(const std::string& root, int64_t before_ms, const std::function<bool(const Row&)>& fn) {
    // 分页（按 修改时间, 路径 递增），每页释放锁，期间其他线程可以写
    int64_t last_m = INT64_MIN;
    std::string last_p;
    for (;;) {
        std::vector<Row> page;
        {
            std::lock_guard<std::recursive_mutex> lk(mu_);
            Stmt q(db_, (std::string("SELECT ") + kCols +
                         " FROM files WHERE root = ? AND state < 3 AND mtime < ? AND (mtime > ? OR (mtime = ? AND path > ?))"
                         " ORDER BY mtime, path LIMIT 500")
                            .c_str());
            q.bind(1, root).bind(2, before_ms).bind(3, last_m).bind(4, last_m).bind(5, last_p);
            while (q.step()) page.push_back(read_row(q));
        }
        if (page.empty()) return;
        last_m = page.back().mtime_ms;
        last_p = page.back().path;
        for (const auto& r : page)
            if (!fn(r)) return;
    }
}

void Index::query(const Filter& f, const std::function<bool(const Row&)>& fn) {
    std::vector<Row> rows;
    {
        std::lock_guard<std::recursive_mutex> lk(mu_);
        Stmt q(db_, (std::string("SELECT ") + kCols +
                     " FROM files WHERE state < 3 AND mtime >= ? AND mtime < ? AND (? = '' OR root = ?)"
                     " AND (? = '' OR verdict = ?) ORDER BY mtime, path")
                        .c_str());
        q.bind(1, f.from_ms).bind(2, f.to_ms).bind(3, f.root).bind(4, f.root).bind(5, f.verdict).bind(6, f.verdict);
        while (q.step()) {
            Row r = read_row(q);
            if (!f.station.empty() && !fmt::glob_match(f.station, r.station)) continue;
            if (!f.product.empty() && !fmt::glob_match(f.product, r.product)) continue;
            if (!f.batch.empty() && !fmt::glob_match(f.batch, r.batch)) continue;
            rows.push_back(std::move(r));
        }
    }
    for (const auto& r : rows)
        if (!fn(r)) return;
}

Totals Index::totals(const std::string& root) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    const auto it = totals_.find(root);
    return it == totals_.end() ? Totals{} : it->second;
}

std::map<std::string, Totals> Index::all_totals() {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    return totals_;
}

std::map<std::string, uint64_t> Index::verdict_counts() {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    std::map<std::string, uint64_t> m;
    Stmt q(db_, "SELECT verdict, COUNT(*) FROM files WHERE state < 3 GROUP BY verdict");
    while (q.step()) m[q.text(0)] = uint64_t(q.i64(1));
    return m;
}

void Index::add_daily(const Daily& d) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    Stmt q(db_,
           "INSERT INTO daily(day, files, bytes_in, bytes_out, deleted, deleted_bytes) VALUES(?,?,?,?,?,?) "
           "ON CONFLICT(day) DO UPDATE SET files = files + excluded.files, bytes_in = bytes_in + excluded.bytes_in, "
           "bytes_out = bytes_out + excluded.bytes_out, deleted = deleted + excluded.deleted, "
           "deleted_bytes = deleted_bytes + excluded.deleted_bytes");
    q.bind(1, d.day.empty() ? local_day(now_ms()) : d.day).bind(2, int64_t(d.files)).bind(3, int64_t(d.bytes_in));
    q.bind(4, int64_t(d.bytes_out)).bind(5, int64_t(d.deleted)).bind(6, int64_t(d.deleted_bytes));
    q.step();
}

std::vector<Daily> Index::daily(int days) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    std::vector<Daily> v;
    Stmt q(db_, "SELECT day, files, bytes_in, bytes_out, deleted, deleted_bytes FROM daily ORDER BY day DESC LIMIT ?");
    q.bind(1, int64_t(days));
    while (q.step())
        v.push_back({q.text(0), uint64_t(q.i64(1)), uint64_t(q.i64(2)), uint64_t(q.i64(3)), uint64_t(q.i64(4)),
                     uint64_t(q.i64(5))});
    return v;
}

std::string Index::meta(const std::string& key) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    Stmt q(db_, "SELECT value FROM meta WHERE key = ?");
    q.bind(1, key);
    return q.step() ? q.text(0) : std::string();
}

void Index::set_meta(const std::string& key, const std::string& value) {
    std::lock_guard<std::recursive_mutex> lk(mu_);
    Stmt q(db_, "INSERT OR REPLACE INTO meta(key, value) VALUES(?, ?)");
    q.bind(1, key).bind(2, value);
    q.step();
}

Index::Tx::Tx(Index& ix) : ix_(ix) {
    ix_.mu_.lock();
    if (ix_.tx_depth_++ == 0) ix_.exec("BEGIN IMMEDIATE");
}

Index::Tx::~Tx() {
    if (--ix_.tx_depth_ == 0) {
        try {
            ix_.exec("COMMIT");
        } catch (const std::exception&) {
            sqlite3_exec(ix_.db_, "ROLLBACK", nullptr, nullptr, nullptr);
        }
    }
    ix_.mu_.unlock();
}

}  // namespace iis::svc
