# C++ Web Crawler

A multi-threaded web crawler with full-text TF-IDF indexing and PageRank scoring.

# Features

| **Multi-threading** | Configurable thread pool (default 8 threads) |
| **BFS Crawling** | Breadth-first URL discovery up to configurable depth |
| **robots.txt** | Fully compliant; per-domain disallow list cached in memory |
| **Politeness** | Per-domain rate limiting (configurable delay) |
| **HTML Parsing** | Regex-based link extraction + visible-text stripping |
| **TF-IDF Index** | Term frequency–inverse document frequency scoring |
| **PageRank** | Damping-factor 0.85, 20-iteration power method |
| **Search** | Interactive CLI search ranked by TF-IDF × PageRank |
| **JSON Export** | Full crawl index saved to `crawl_index.json` |

# Build

```bash
# Install dependency (Ubuntu/Debian)
sudo apt-get install libcurl4-openssl-dev

# Compile
g++ -std=c++17 -O2 -pthread webcrawler.cpp -lcurl -o webcrawler
```

# How to Use

```bash
./webcrawler <seed_url> [max_pages] [threads] [max_depth]
```

## Examples

```bash
# Crawl up to 200 pages starting from Wikipedia, 8 threads, depth 3
./webcrawler https://en.wikipedia.org/wiki/Web_crawler 200 8 3

# Small test — 20 pages, 4 threads, depth 2
./webcrawler https://example.com 20 4 2

# Deep crawl — 500 pages, 16 threads, depth 5
./webcrawler https://news.ycombinator.com 500 16 5
```

# After crawling — interactive search

```
Search> web crawler algorithms
  [1] Web crawler - Wikipedia
      https://en.wikipedia.org/wiki/Web_crawler
      Score: 0.003421

  [2] ...
```

Type `quit` to exit the search loop.

# Architecture

```
Seed URL
   │
   ▼
CrawlQueue (BFS)
   │
   ├── Thread 1 ──► HttpFetcher ──► parseHtml ──► SearchIndex
   ├── Thread 2 ──► HttpFetcher ──► parseHtml ──► SearchIndex
   ├── ...         RateLimiter
   └── Thread N ──► RobotsCache
                         │
                    PageRank (post-crawl)
                         │
                    Interactive Search
```

# Output Files

- `crawl_index.json` — all crawled pages with metadata and PageRank scores

# Configuration (in code / CLI)

| Parameter | Default | Description |

| `max_pages` | 200 | Stop after N pages indexed |
| `max_threads` | 8 | Worker thread count |
| `max_depth` | 3 | BFS depth limit from seed |
| `timeout_sec` | 10 | HTTP request timeout |
| `delay_ms` | 300 | Per-domain politeness delay (ms) |
| `same_domain` | false | Restrict to seed domain only |