#include <iostream>
#include <string>
#include <vector>
#include <queue>
#include <unordered_set>
#include <unordered_map>
#include <set>
#include <map>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <regex>
#include <sstream>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <curl/curl.h>

static std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream ss;
    ss << std::put_time(std::localtime(&t), "%H:%M:%S");
    return ss.str();
}

struct HttpResponse {
    long status_code = 0;
    std::string content_type;
    std::string body;
    std:: string final_url;
    bool ok = false;
};

static size_t curlWriteCallback(void* ptr, size_t size, size_t nmemb, std::string* data) {
    data->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}
static size_t curlHeaderCallback(void* ptr, size_t size, size_t nmemb, std::string* data) {
    data->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}

class HttpFetcher {
    public:
        HttpFetcher() {
            curl_global_init(CURL_GLOBAL_DEFAULT);
        }
        ~HttpFetcher() {
            curl_global_cleanup();
        }

        HttpResponse fetch(const std::string& url, int timeout_sec = 10) {
            HttpResponse resp;
            CURL* curl = curl_easy_init();
            if(!curl) return resp;

            std::string body, headers;
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
            curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curlHeaderCallback);
            curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headers);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout_sec);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "MyCrawler/1.0");
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0l);

            CURLcode res = curl_easy_perform(curl);
            if(res == CURLE_OK) {
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.status_code);
                char* final = nullptr;
                curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &final);
                if(final) resp.final_url = final;
                resp.body = std::move(body);
                resp.ok = (resp.status_code >= 200 && resp.status_code < 400);

                std::istringstream hs(headers);
                std::string line;
                while(std::getline(hs, line)) {
                    if(line.find("content-type:") != std::string::npos || line.find("content-Type:") != std::string::npos) {
                        auto colon = line.find(":");
                        if(colon != std::string::npos) resp.content_type = line.substr(colon+1);
                        resp.content_type.erase(std::remove_if(resp.content_type.begin(), resp.content_type.end(), [](char c){ return c == '\r' || c == '\n'; }), resp.content_type.end());
                    }
                }
            }
            curl_easy_cleanup(curl);
            return resp;

        }
};

struct ParsedUrl {
    std::string scheme;
    std::string host;
    std::string port;
    std::string path;
    std::string query;
};

ParsedUrl parseUrl(const std::string& url) {
    ParsedUrl p;
    static std::regex re(R"(^(https?)://([^/:?#]+)(?::(\d+))?([^?#]*)(?:\?([^#]*))?(?:#.*)?)");

    std::smatch m;
    if(std::regex_match(url, m, re)) {
        p.scheme = m[1];
        p.host = m[2];
        p.port = m[3];
        p.path = (m[4].str()).empty() ? "/" : m[4].str();
        p.query = m[5];
    }

    return p;
}

std::string getOrigin(const std::string& url) {
    auto p = parseUrl(url);
    if(p.host.empty()) return "";
    std::string origin = p.scheme + "://" + p.host;
    if(!p.port.empty()) origin += ":" + p.port;
    return origin;
}

std::string getDomain(const std::string& url) {
    return parseUrl(url).host;
}

std::string resolveUrl(const std::string& base, const std::string& href) {
    if(href.empty()) return "";
    if(href.rfind("http://", 0) == 0 || href.rfind("https://", 0) == 0) return href;
    if(href[0] == '#') return "";
    if(href.rfind("mailto:", 0) == 0 || href.rfind("javascript:", 0) == 0) return "";

    auto p = parseUrl(base);
    if(p.host.empty()) return "";

    std::string origin = p.scheme + "://" + p.host;
    if(!p.port.empty()) origin += ":" + p.port;

    if(href.rfind("//", 0) == 0) return p.scheme + ":" + href;
    if(href[0] == '/') return origin + href;

    std::string dir = p.path;
    auto slash = dir.rfind('/');
    if(slash != std::string::npos) dir = dir.substr(0, slash+1);
    else dir = "/";

    return origin + dir + href;

}

std::string normaliseUrl(const std::string& url) {
    auto hash = url.find('#');
    std::string u = (hash != std::string::npos) ? url.substr(0, hash) : url;
    if(!u.empty() && u.back() == '?') u.pop_back();
    return u;
}

struct PageData {
    std::string title;
    std::string text;
    std::vector<std::string> links;
    std::string description;
};

PageData parseHtml(const std::string& html, const std::string& base_url) {
    PageData data;

    {
        std::regex re(R"(<title[^>]*>([\s\S]*?)</title>)", std::regex::icase);
        std::smatch m;
        if(std::regex_search(html, m, re)) data.title = m[1];
    }

    {
        std::regex re(R"(<meta[^>]+name=["\']description["\'][^>]+content=["\']([^"\']*)["\'])", std::regex::icase);
        std::smatch m;
        if(std::regex_search(html, m, re)) data.description = m[1];
    }

    {
        std::regex re(R"(href\s*=\s*["\']([^"\'#][^"\']*)["\'])", std::regex::icase);
        auto begin = std::sregex_iterator(html.begin(), html.end(), re);
        auto end = std::sregex_iterator();
        for(auto it = begin; it != end; ++it) {
            std::string href = (*it)[1].str();
            std::string resolved = resolveUrl(base_url, href);
            if(!resolved.empty()) data.links.push_back(normaliseUrl(resolved));
        }
    }

    {
        std::string stripped;
        stripped.reserve(html.size() / 2);
        bool in_tag = false;
        bool in_script = false;
        bool in_style = false;

        for(size_t i = 0; i < html.size(); ++i) {
            char c = html[i];
            if(!in_tag) {
                if(c == '<') {
                    in_tag = true;

                    std::string tag;
                    for(size_t j = i+1; j < std::min(i+10, html.size()) && html[j] != '>' && html[j] != ' '; ++j) tag += std::tolower(html[j]);
                    if(tag == "script") in_style = true;
                    if(tag == "style") in_style = true;
                }else if (!in_script && !in_style) {
                    stripped += c;
                }
            } else {
                if(c == '>') {
                    std::string tag;
                    for(size_t j = i-5; j < i && j < html.size(); ++j) {
                        if(html[j] != ' ' && html[j] != '>') tag += std::tolower(html[j]);
                    }
                    in_tag = false;
                    if(in_script && tag.find("/script") != std::string::npos) in_script = false;
                    if(in_style && tag.find("/style") != std::string::npos) in_style = false;
                }
            }
        }
        std::regex wsre(R"(\s+)");
        data.text = std::regex_replace(stripped, wsre, " ");
    }

    return data;
}

class RobotsCache {
    public:
        bool isAllowed(const std::string& url, HttpFetcher& fetcher) {
            std::string origin = getOrigin(url);
            if(origin.empty()) return false;

            std::lock_guard<std::mutex> lock(mutex_);
            auto it = cache_.find(origin);
            if(it == cache_.end()) {
                std::string robotsUrl = origin + "/robots.txt";
                auto resp = fetcher.fetch(robotsUrl, 5);
                cache_[origin] = parseRobots(resp.ok ? resp.body : "");
            }

            const auto& disallowed = cache_[origin];
            std::string path = parseUrl(url).path;
            for(const auto& d : disallowed) {
                if(path.rfind(d, 0) == 0) return false;
            }
            return true;
        }
    
    private: 
        std::vector<std::string> parseRobots(const std::string& body) {
            std::vector<std::string> disallowed;
            bool our_agent = false;
            std::istringstream ss(body);
            std::string line;
            while(std::getline(ss, line)) {
                auto hash = line.find('#');
                if(hash != std::string::npos) line = line.substr(0, hash);
                line.erase(0, line.find_first_not_of(" \t\r\n"));
                line.erase(line.find_last_not_of(" \t\r\n") + 1);
                if(line.empty()) {
                    our_agent = false;
                    continue;
                }
                std::string lower = line;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

                if(lower.rfind("user-agent:", 0) == 0) {
                    std::string agent = lower.substr(11);
                    agent.erase(0, agent.find_first_not_of(" \t"));
                    our_agent = (agent == "*" || agent.find("mycrawler") != std::string::npos);
                } else if(our_agent && lower.rfind("disallow:", 0) == 0) {
                    std::string path = line.substr(9);
                    path.erase(0, path.find_first_not_of(" \t"));
                    if(!path.empty()) disallowed.push_back(path);
                }
            }
            return disallowed;
        }

        std::mutex mutex_;
        std::unordered_map<std::string, std::vector<std::string>> cache_;
};

class ThreadPool {
    public:
        explicit ThreadPool(size_t n) : stop_(false) {
            for(size_t i = 0; i < n; ++i) {
                workers_.emplace_back([this]{ workerLoop(); });
            }
        }

        ~ThreadPool() {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                stop_ = true;
            }
            cv_.notify_all();
            for(auto& t : workers_) t.join();
        }

        void enqueue(std::function<void()> task) {  
            {
                std::unique_lock<std::mutex> lonk(mutex_);
                tasks_.push(std::move(task));
            }
            cv_.notify_one();
        }

        size_t pendingTasks() {
            std::unique_lock<std::mutex> lock(mutex_);
            return tasks_.size();
        }
    
    private:
        void workerLoop() {
            while(true) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cv_.wait(lock, [this]{ return stop_ || !tasks_.empty(); });

                    if(stop_ && tasks_.empty()) return;
                    task = std::move(tasks_.front());
                    tasks_.pop();
                }
                task();
            }
        }

        std::vector<std::thread> workers_;
        std::queue<std::function<void()>> tasks_;
        std::mutex mutex_;
        std::condition_variable cv_;
        bool stop_;
};

struct IndexedPage {
    std::string url;
    std::string title;
    std::string description;
    double pagerank_score = 1.0;
    int inlink_count = 0;
    std::map<std::string, int> term_freq;
    int total_terms = 0;
};

class SearchIndex {
    public: 
        void addPage(const std::string& url, const std::string& title, const std::string& description, const std::string& text) {
            std::lock_guard<std::mutex> lock(mutex_);
            IndexedPage page;
            page.url = url;
            page.title = title;
            page.description = description;

            std::string lower = text;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            std::regex token_re(R"([a-z]{3,})");
            auto begin = std::sregex_iterator(lower.begin(), lower.end(), token_re);
            auto end = std::sregex_iterator();
            for(auto it = begin; it != end; ++it) {
                std::string w = (*it)[0];
                if(!isStopWord(w)) {
                    page.term_freq[w]++;
                    page.total_terms++;
                    doc_freq_[w]++;
                }
            }
            pages_.push_back(std::move(page));
            url_to_idx_[url] = pages_.size()-1;

        }

        void addLink(const std::string& from, const std::string& to) {
            std::lock_guard<std::mutex> lock(mutex_);
            outlinks_[from].insert(to);
            inlinks_[to].insert(from);
            if(url_to_idx_.count(to)) pages_[url_to_idx_[to]].inlink_count++;
        }

        void computePageRank() {
            std::lock_guard<std::mutex> lock(mutex_);
            int N = pages_.size();
            if(N == 0) return;
            std::vector<double> pr(N, 1.0/N);
            std::vector<double> pr_new(N);
            double d = 0.85;

            for(int iter = 0; iter < 20; ++iter) {
                std::fill(pr_new.begin(), pr_new.end(), (1.0-d)/N);
                for(auto& [from, tos] : outlinks_) {
                    auto it = url_to_idx_.find(from);
                    if(it == url_to_idx_.end()) continue;
                    int fi = it->second;
                    double share = pr[fi] / std::max(1, (int)tos.size());
                    for(auto& to : tos) {
                        auto jt = url_to_idx_.find(to);
                        if(jt != url_to_idx_.end()) pr_new[jt->second] += d * share;
                    }
                }
                pr = pr_new;
            }
            for(int i = 0; i < N; i++) pages_[i].pagerank_score = pr[i];
        }

        struct SearchResult {
            std::string url, title, description;
            double score;
        };

        std::vector<SearchResult> search(const std::string& query, int top_k = 10) {
            std::lock_guard<std::mutex> lock(mutex_);
            std::string lower = query;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            std::regex re(R"([a-z]{3,})");
            std::vector<std::string> terms;
            auto begin = std::sregex_iterator(lower.begin(), lower.end(), re);
            auto end = std::sregex_iterator();
            for(auto it = begin; it != end; ++it) terms.push_back((*it)[0]);

            int N = pages_.size();
            std::vector<std::pair<double, int>> scored;
            for(int i = 0; i < N; ++i) {
                double score = 0;
                for(auto& term : terms) {
                    auto tf_it = pages_[i].term_freq.find(term);
                    if(tf_it == pages_[i].term_freq.end()) continue;
                    double tf = (double)tf_it->second / std::max(1, pages_[i].total_terms);
                    double df = doc_freq_.count(term) ? doc_freq_[term] : 1;
                    double idf = std::log((double)N / df + 1.0);
                    score += tf * idf;
                }
                score *= (1.0 + std::log(1.0 + pages_[i].pagerank_score * 1000));
                if (score > 0) scored.emplace_back(score, i);
            }
            std::partial_sort(scored.begin(), scored.begin()+std::min((int)scored.size(), top_k), scored.end(), [](auto& a, auto& b){ return a.first > b.first; });

            std::vector<SearchResult> results;
            for(int i = 0; i < std::min((int)scored.size(), top_k); ++i) {
                auto& p = pages_[scored[i].second];
                results.push_back({p.url, p.title, p.description, scored[i].first});
            }
            return results;
        }
        
        int pageCount() {
            std::lock_guard<std::mutex> lock(mutex_);
            return pages_.size();
        }

        void exportJson(const std::string& path) {
            std::lock_guard<std::mutex> lock(mutex_);
            std::ofstream f(path);
            f << "[\n";
            for(size_t i = 0; i < pages_.size(); ++i) {
                auto& p = pages_[i];
                auto esc = [](std::string s) {
                    std::string r;
                    for(char c : s) {
                        if(c == '"') r += "\\\"";
                        else if (c == '\\') r += "\\\\";
                        else if (c == '\n') r += "\\n";
                        else if (c == '\r') r += "";
                        else r += c;
                    }
                    return r;
                };
                f << "  {\n";
                f << "    \"url\": \""         << esc(p.url)         << "\",\n";
                f << "    \"title\": \""       << esc(p.title)       << "\",\n";
                f << "    \"description\": \"" << esc(p.description) << "\",\n";
                f << "    \"pagerank\": "      << p.pagerank_score   << ",\n";
                f << "    \"inlinks\": "       << p.inlink_count     << ",\n";
                f << "    \"terms\": "         << p.total_terms      << "\n";
                f << "  }";
                if (i + 1 < pages_.size()) f << ",";
                f << "\n";
            }
            f << "]\n";
        }
    
    private: 
        bool isStopWord(const std::string& s) {
            static const std::unordered_set<std::string> stop = {"the","and","for","are","but","not","you","all","can","had","her","was",
            "one","our","out","day","get","has","him","his","how","its","let","may",
            "nor","now","old","see","two","way","who","boy","did","does","from","have",
            "that","this","with","they","been","just","into","than","then","them","these",
            "some","such","when","will","your","each","from","here","more","also","which"};
            return stop.count(s) > 0;
        }
    
        std::mutex mutex_;
        std::vector<IndexedPage> pages_;
        std::unordered_map<std::string, size_t> url_to_idx_;
        std::unordered_map<std::string, int> doc_freq_;
        std::unordered_map<std::string, std::set<std::string>> outlinks_;
        std::unordered_map<std::string, std::set<std::string>> inlinks_;
};

struct CrawlTask {
    std::string url;
    int depth;
};

class CrawlQueue {
    public:
        void push(const CrawlTask& t) {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(t);
            cv_.notify_one();
        }

        bool pop(CrawlTask& out, int timeout_ms = 500) {
            std::unique_lock<std::mutex> lock(mutex_);
            if(cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this]{ return !queue_.empty(); })) {
                out = queue_.front();
                queue_.pop();
                return true;
            }
            return false;
        }

        bool empty() {
            std::lock_guard<std::mutex> lock(mutex_);
            return queue_.empty();
        }

        size_t size() {
            std::lock_guard<std::mutex> loc(mutex_);
            return queue_.size();
        }

    private:
        std::queue<CrawlTask> queue_;
        std::mutex mutex_;
        std::condition_variable cv_;
};

class RateLimiter {
    public:
        void waitIfNeeded(const std::string& domain, int delay_ms = 500) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto now = std::chrono::steady_clock::now();
            auto& last = last_access_[domain];
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
            if(elapsed < delay_ms) {
                int wait = delay_ms - (int)elapsed;
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(wait));
                lock.lock();
            }
            last_access_[domain] = std::chrono::steady_clock::now();
        }

    private:
        std::mutex mutex_;
        std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_access_;
};

class WebCrawler {
    public: 
        struct Config {
            std::string seed_url;
            int max_pages = 200;
            int max_threads = 8;
            int max_depth = 3;
            int timeout_sec = 10;
            int delay_ms = 300;
            bool same_domain = false;
        };

        explicit WebCrawler(Config cfg) : cfg_(std::move(cfg)) {}

        void run() {
            auto start = std::chrono::steady_clock::now();

            std::cout << "\n";
            std::cout << "\n";
            std::cout << "C++ Web Crawler 1.0 \n";
            std::cout << "\n";
            std::cout << "\n";
            std::cout << "[" << timestamp() << "] Seed URL: " << cfg_.seed_url << "\n";
            std::cout << "[" << timestamp() << "] Max Pages: " << cfg_.max_pages << "\n";
            std::cout << "[" << timestamp() << "] Threads: " << cfg_.max_threads << "\n";
            std::cout << "[" << timestamp() << "] Max depth: " << cfg_.max_depth << "\n";

            queue_.push({cfg_.seed_url, 0});
            visited_.insert(cfg_.seed_url);
            std::string seed_domain = getDomain(cfg_.seed_url);

            std::atomic<int> crawled{0};
            std::atomic<int> failed{0};
            std::atomic<int> done{false};

            auto worker = [&](){
                HttpFetcher fetcher;
                while(!done) {
                    CrawlTask task;
                    if(!queue_.pop(task, 300)) {
                        if(crawled >= cfg_.max_pages) done = true;
                        continue;
                    }
                    if(crawled >= cfg_.max_pages) {
                        done = true;
                        continue;
                    }
                    if(cfg_.same_domain && getDomain(task.url) != seed_domain) {
                        continue;
                    }
                    if(!robots_.isAllowed(task.url, fetcher)) {
                        std::cout << "[" << timestamp() << "] ROBOTS" << task.url << "\n";
                        continue;
                    }
                    rate_.waitIfNeeded(getDomain(task.url), cfg_.delay_ms);

                    auto resp = fetcher.fetch(task.url, cfg_.timeout_sec);
                    if(!resp.ok) {
                        failed++;
                        std::cout << "[" << timestamp() << "] FAIL(" << resp.status_code << ") " << task.url << "\n";
                        continue;
                    }

                    if(resp.content_type.find("text/html") == std::string::npos && resp.content_type.find("text/plain") == std::string::npos && !resp.content_type.empty()) { continue; }

                    auto page = parseHtml(resp.body, task.url);
                    int n = ++crawled;

                    index_.addPage(task.url, page.title, page.description, page.text);

                    std::cout << "[" << timestamp() << "] [" << std::setw(3) << n << "/" << cfg_.max_pages << "] D" << task.depth << " " << task.url.substr(0, 80) << "\n";

                    if(task.depth < cfg_.max_depth && crawled < cfg_.max_pages) {
                        std::lock_guard<std::mutex> lock(visit_mutex_);
                        for(auto& link : page.links) {
                            if(link.empty()) continue;
                            if(visited_.size() > (size_t)cfg_.max_pages * 5) break;
                            if(!visited_.count(link)) {
                                visited_.insert(link);
                                index_.addLink(task.url, link);
                                queue_.push({link, task.depth+1});
                            }
                        }
                    }
                }
            };

            std::vector<std::thread> threads;
            for(int i = 0; i < cfg_.max_threads; ++i) {
                threads.emplace_back(worker);
            }
            for(auto& t : threads) t.join();

            std::cout << "\n[" << timestamp() << "] Computing PageRank...\n";
            index_.computePageRank();

            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();

            std::cout << "\n";
            std::cout << "\n";
            std::cout << "Craw Complete\n";
            std::cout << "\n";
            std::cout << " Pages Crawled : " << crawled << "\n";
            std::cout << " Failed : " << failed << "\n";
            std::cout << " Time : " << elapsed << "s\n\n";

            std::string out_path = "crawl_index.json";
            index_.exportJson(out_path);
            std::cout << "[" << timestamp() << "] Index exported to " << out_path << "\n\n";

            searchLoop();
        }

        void searchLoop() {
            std::cout << "\n";
            std::cout << "Search the Index\n";
            std::cout << "Type a query and press Enter (or 'quit to exit).\n\n";

            std::string query;
            while(true) {
                std::cout << "Search> ";
                std::getline(std::cin, query);
                if(query == "quit" || query == "exit" || query == "q") break;
                if(query.empty()) continue;

                auto results = index_.search(query, 10);
                if(results.empty()) {
                    std::cout << " No results found.\n\n";
                    continue;
                }
                std::cout << "\n Top " << results.size() << " results for \"" << query << "\":\n\n";
                for(size_t i = 0; i < results.size(); ++i) {
                    auto& r = results[i];
                    std::string title = r.title.empty() ? "(no title)" : r.title;
                    if(title.size() > 60) title = title.substr(0, 57) + "...";
                    std::string desc = r.description.empty() ? r.url : r.description;
                    if(desc.size() > 100) desc = desc.substr(0, 97) + "...";
                    std::cout << " [" << (i+1) << "] " << title << "\n";
                    std::cout << "     " << r.url << "\n";
                    std::cout << "     " << desc << "\n";
                    std::cout << "     Score: " << std::fixed << std::setprecision(6) << r.score << "\n\n";
                }
            }
        }

    private:
        Config cfg_;
        CrawlQueue queue_;
        SearchIndex index_;
        RobotsCache robots_;
        RateLimiter rate_;

        std::unordered_set<std::string> visited_;
        std::mutex visit_mutex_;
};


int main(int argc, char** argv) {
    if(argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <seed_url> [max_pages=200] [threads=8] [max_depth=3]\n";
        std::cerr << "Example: " << argv[0] << " https://en.wikipedia.org/wiki/Web_crawler 200 8 3\n";
        return 1;
    }

    WebCrawler::Config cfg;
    cfg.seed_url = argv[1];
    cfg.max_pages = (argc > 2) ? std::stoi(argv[2]) : 200;
    cfg.max_threads = (argc > 3) ? std::stoi(argv[3]) : 8;
    cfg.max_depth = (argc > 4) ? std::stoi(argv[4]) : 3;

    WebCrawler crawler(cfg);
    crawler.run();
    return 0;
}