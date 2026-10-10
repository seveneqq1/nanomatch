#include "core/order_book.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#if defined(__APPLE__)
#include <time.h>
#endif

namespace {

constexpr uint32_t kOps     = 1'000'000;
constexpr uint32_t kWarmup  = 50'000;
constexpr uint32_t kSeed    = 32'768;
constexpr uint32_t kBins    = 96;

[[nodiscard]] uint64_t nanos() noexcept {
#if defined(__APPLE__)
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull +
           static_cast<uint64_t>(ts.tv_nsec);
#endif
}

struct Rng {
    uint64_t s;
    uint64_t next() noexcept {
        uint64_t x = s;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        s = x;
        return x * 0x2545F4914F6CDD1Dull;
    }
    uint32_t u32() noexcept { return static_cast<uint32_t>(next() >> 32); }
};

[[nodiscard]] nanomatch::Command make_cmd(uint64_t id, Rng& rng, uint64_t max_id) {
    nanomatch::Command c{};
    c.order_id = id;
    c.user_id  = 1 + (rng.u32() % 255);
    const uint32_t r = rng.u32() % 100;
    if (r < 25 && max_id > 1) {
        c.type     = nanomatch::CommandType::Cancel;
        c.order_id = 1 + (rng.next() % max_id);
        return c;
    }
    if (r < 40) {
        c.type = nanomatch::CommandType::Market;
        c.side = (rng.u32() & 1) ? nanomatch::Side::Buy : nanomatch::Side::Sell;
        c.qty  = 1 + (rng.u32() % 8);
        return c;
    }
    c.type  = nanomatch::CommandType::Limit;
    c.qty   = 1 + (rng.u32() % 16);
    c.side  = (rng.u32() & 1) ? nanomatch::Side::Buy : nanomatch::Side::Sell;
    const bool aggressive = (rng.u32() % 5) == 0;
    if (c.side == nanomatch::Side::Buy) {
        c.price = aggressive ? 10'080 + static_cast<int64_t>(rng.u32() % 40)
                             : 10'000 + static_cast<int64_t>(rng.u32() % 64);
    } else {
        c.price = aggressive ? 10'000 + static_cast<int64_t>(rng.u32() % 40)
                             : 10'064 + static_cast<int64_t>(rng.u32() % 64);
    }
    return c;
}

[[nodiscard]] uint64_t percentile(const uint64_t* s, uint32_t n, double p) {
    if (n == 0) {
        return 0;
    }
    const double  idx = p / 100.0 * static_cast<double>(n - 1);
    const uint32_t lo = static_cast<uint32_t>(idx);
    const uint32_t hi = std::min(n - 1, lo + 1);
    const double   f  = idx - static_cast<double>(lo);
    return static_cast<uint64_t>(static_cast<double>(s[lo]) * (1.0 - f) +
                                 static_cast<double>(s[hi]) * f);
}

void write_json(FILE* f, uint32_t n, uint64_t p50, uint64_t p90, uint64_t p99,
                uint64_t p999, uint64_t mean, uint64_t mn, uint64_t mx,
                double thr, const uint32_t* bins, uint64_t bin_w, uint64_t cap,
                uint64_t trades, uint32_t resting) {
    std::fprintf(f, "{\n");
    std::fprintf(f, "  \"engine\": \"nanomatch\",\n");
    std::fprintf(f, "  \"ops\": %u,\n", n);
    std::fprintf(f, "  \"unit\": \"ns\",\n");
    std::fprintf(f, "  \"trades\": %llu,\n", static_cast<unsigned long long>(trades));
    std::fprintf(f, "  \"resting\": %u,\n", resting);
    std::fprintf(f, "  \"throughput_ops_s\": %.3f,\n", thr);
    std::fprintf(f, "  \"min\": %llu,\n", static_cast<unsigned long long>(mn));
    std::fprintf(f, "  \"mean\": %llu,\n", static_cast<unsigned long long>(mean));
    std::fprintf(f, "  \"max\": %llu,\n", static_cast<unsigned long long>(mx));
    std::fprintf(f, "  \"percentiles\": {\"p50\": %llu, \"p90\": %llu, \"p99\": %llu, \"p99.9\": %llu},\n",
                 static_cast<unsigned long long>(p50), static_cast<unsigned long long>(p90),
                 static_cast<unsigned long long>(p99), static_cast<unsigned long long>(p999));
    std::fprintf(f, "  \"histogram\": {\"bin_width_ns\": %llu, \"cap_ns\": %llu, \"counts\": [",
                 static_cast<unsigned long long>(bin_w), static_cast<unsigned long long>(cap));
    for (uint32_t i = 0; i < kBins; ++i) {
        std::fprintf(f, "%u%s", bins[i], i + 1 == kBins ? "" : ",");
    }
    std::fprintf(f, "]}\n}\n");
}

}  // namespace

int main(int argc, char** argv) {
    const char* html_path = argc > 1 ? argv[1] : "latency_report.html";
    const char* json_path = argc > 2 ? argv[2] : "latency_report.json";

    using Book = nanomatch::OrderBook<(1u << 20), (1u << 14), (1u << 12), (1u << 16)>;
    std::printf("nanomatch bench  allocating book (orders=%u)…\n", Book::max_orders());
    auto* book = new Book();

    Rng rng{0xC0FFEEULL ^ 0x9E3779B97F4A7C15ULL};

    uint64_t next_id = 1;
    for (uint32_t i = 0; i < kSeed; ++i) {
        nanomatch::Command c{};
        c.order_id = next_id++;
        c.user_id  = 1;
        c.type     = nanomatch::CommandType::Limit;
        c.qty      = 4;
        if (i & 1) {
            c.side  = nanomatch::Side::Buy;
            c.price = 10'000 + static_cast<int64_t>(i % 64);
        } else {
            c.side  = nanomatch::Side::Sell;
            c.price = 10'064 + static_cast<int64_t>(i % 64);
        }
        book->process(c);
    }

    auto drain = [&]() {
        nanomatch::Trade           tr{};
        nanomatch::ExecutionReport er{};
        while (book->try_pop_trade(tr)) {
        }
        while (book->try_pop_exec(er)) {
        }
    };
    drain();

    std::vector<nanomatch::Command> cmds(kWarmup + kOps);
    for (uint32_t i = 0; i < kWarmup + kOps; ++i) {
        const uint64_t id = next_id++;
        cmds[i]           = make_cmd(id, rng, id - 1);
        if (cmds[i].type != nanomatch::CommandType::Cancel) {
            cmds[i].order_id = id;
        }
    }

    for (uint32_t i = 0; i < kWarmup; ++i) {
        book->process(cmds[i]);
        if ((i & 1023u) == 1023u) {
            drain();
        }
    }
    drain();

    auto* samples = static_cast<uint64_t*>(
        ::operator new(sizeof(uint64_t) * kOps, std::align_val_t{64}));

    const uint64_t t_begin = nanos();
    for (uint32_t i = 0; i < kOps; ++i) {
        const uint64_t t0 = nanos();
        book->process(cmds[kWarmup + i]);
        const uint64_t t1 = nanos();
        samples[i]        = t1 - t0;
        drain();
    }
    const uint64_t t_end = nanos();

    uint64_t sum = 0, mn = ~uint64_t{0}, mx = 0;
    for (uint32_t i = 0; i < kOps; ++i) {
        sum += samples[i];
        mn = std::min(mn, samples[i]);
        mx = std::max(mx, samples[i]);
    }
    std::sort(samples, samples + kOps);
    const uint64_t p50  = percentile(samples, kOps, 50.0);
    const uint64_t p90  = percentile(samples, kOps, 90.0);
    const uint64_t p99  = percentile(samples, kOps, 99.0);
    const uint64_t p999 = percentile(samples, kOps, 99.9);
    const uint64_t mean = sum / kOps;
    const double   thr =
        (t_end > t_begin) ? (1e9 * static_cast<double>(kOps) / static_cast<double>(t_end - t_begin))
                          : 0.0;

    const uint64_t cap   = std::max<uint64_t>(p999 * 2 + 1, 64);
    const uint64_t bin_w = std::max<uint64_t>(1, (cap + kBins - 1) / kBins);
    uint32_t       bins[kBins]{};
    for (uint32_t i = 0; i < kOps; ++i) {
        uint32_t b = static_cast<uint32_t>(samples[i] / bin_w);
        if (b >= kBins) {
            b = kBins - 1;
        }
        ++bins[b];
    }

    std::printf("\n");
    std::printf("  ops         %u\n", kOps);
    const double match_thr = mean ? (1e9 / static_cast<double>(mean)) : 0.0;
    std::printf("  throughput  %.2f ops/s  (%.2f M/s wall, %.2f M/s from mean)\n",
                thr, thr / 1e6, match_thr / 1e6);
    std::printf("  min         %llu ns\n", static_cast<unsigned long long>(mn));
    std::printf("  mean        %llu ns\n", static_cast<unsigned long long>(mean));
    std::printf("  p50         %llu ns\n", static_cast<unsigned long long>(p50));
    std::printf("  p90         %llu ns\n", static_cast<unsigned long long>(p90));
    std::printf("  p99         %llu ns\n", static_cast<unsigned long long>(p99));
    std::printf("  p99.9       %llu ns\n", static_cast<unsigned long long>(p999));
    std::printf("  max         %llu ns\n", static_cast<unsigned long long>(mx));
    std::printf("  trades      %llu\n", static_cast<unsigned long long>(book->trade_count()));
    std::printf("  resting     %u\n", book->resting());
    std::printf("  rejects     %llu\n", static_cast<unsigned long long>(book->reject_count()));
    std::printf("  dropped     %u  (full output rings; matching still applied)\n",
                book->dropped_reports());

    char bin_csv[kBins * 12]{};
    {
        char*  p   = bin_csv;
        size_t rem = sizeof(bin_csv);
        for (uint32_t i = 0; i < kBins; ++i) {
            const int n = std::snprintf(p, rem, "%u%s", bins[i], i + 1 == kBins ? "" : ",");
            if (n < 0 || static_cast<size_t>(n) >= rem) {
                break;
            }
            p += n;
            rem -= static_cast<size_t>(n);
        }
    }

    if (FILE* hf = std::fopen(html_path, "w")) {
        std::fprintf(hf, R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>nanomatch latency</title>
<style>
  html, body { margin: 0; background: #1a120c; color: #efe4cc;
    font: 15px/1.45 "SF Mono", Menlo, Consolas, ui-monospace, monospace; }
  body { min-height: 100vh; padding: 32px 28px 48px; }
  h1 { font-weight: 500; font-size: 22px; letter-spacing: -0.02em; margin: 0 0 6px; }
  .sub { color: #b8a888; margin-bottom: 28px; max-width: 68ch; }
  .ticker { display: flex; flex-wrap: wrap; border: 1px solid #5a4630; margin-bottom: 24px; }
  .ticker div { padding: 14px 18px; border-right: 1px solid #5a4630; min-width: 132px; }
  .ticker div:last-child { border-right: 0; }
  .ticker span { display: block; color: #b8a888; font-size: 11px; margin-bottom: 4px; }
  .ticker strong { font-size: 20px; font-weight: 500; color: #ff7a45; }
  canvas { width: 100%%; height: 360px; background: #120c08; border: 1px solid #5a4630; display: block; }
  figcaption { color: #b8a888; font-size: 12px; margin-top: 8px; }
</style>
</head>
<body>
<h1>nanomatch · matching latency</h1>
<p class="sub">One million mixed limit, market and cancel commands on the zero-allocation hot path.
Times are monotonic-raw nanoseconds around <code>OrderBook::process</code>.</p>
<div class="ticker">
  <div><span>p50</span><strong>%llu ns</strong></div>
  <div><span>p90</span><strong>%llu ns</strong></div>
  <div><span>p99</span><strong>%llu ns</strong></div>
  <div><span>p99.9</span><strong>%llu ns</strong></div>
  <div><span>mean</span><strong>%llu ns</strong></div>
  <div><span>throughput</span><strong>%.2f M/s</strong></div>
</div>
<canvas id="h" width="1400" height="360"></canvas>
<figcaption>min %llu ns · max %llu ns · %u ops · %llu trades · %u resting · bin %llu ns</figcaption>
<script>
const bins = [%s];
const p = {p50:%llu,p90:%llu,p99:%llu,p999:%llu};
const binW = %llu;
const canvas = document.getElementById('h');
const ctx = canvas.getContext('2d');
const w = canvas.width, h = canvas.height;
const max = Math.max.apply(null, bins) || 1;
ctx.fillStyle = '#120c08'; ctx.fillRect(0,0,w,h);
ctx.strokeStyle = 'rgba(239,228,204,0.08)';
for (let i = 0; i < 6; i++) {
  const y = (h-28)*i/5+8;
  ctx.beginPath(); ctx.moveTo(40,y); ctx.lineTo(w-8,y); ctx.stroke();
}
const bw = (w-56)/bins.length;
ctx.fillStyle = '#ff7a45';
for (let i = 0; i < bins.length; i++) {
  const bh = (bins[i]/max)*(h-48);
  ctx.fillRect(48+i*bw, h-24-bh, Math.max(1,bw-1), bh);
}
let acc=0, total=bins.reduce((a,b)=>a+b,0)||1;
ctx.beginPath(); ctx.strokeStyle='#efe4cc'; ctx.lineWidth=1.5;
for (let i=0;i<bins.length;i++){
  acc+=bins[i];
  const x=48+i*bw, y=8+(1-acc/total)*(h-48);
  if(i===0) ctx.moveTo(x,y); else ctx.lineTo(x,y);
}
ctx.stroke();
function mark(ns,label){
  const x=48+(ns/binW)*bw;
  ctx.setLineDash([3,4]); ctx.strokeStyle='#efe4cc';
  ctx.beginPath(); ctx.moveTo(x,8); ctx.lineTo(x,h-24); ctx.stroke();
  ctx.setLineDash([]); ctx.fillStyle='#efe4cc';
  ctx.font='12px ui-monospace,monospace'; ctx.fillText(label,x+4,20);
}
mark(p.p50,'p50'); mark(p.p90,'p90'); mark(p.p99,'p99'); mark(p.p999,'p99.9');
ctx.fillStyle='#b8a888'; ctx.font='11px ui-monospace,monospace';
ctx.fillText('latency →', w-90, h-8);
</script>
</body></html>
)HTML",
                     static_cast<unsigned long long>(p50), static_cast<unsigned long long>(p90),
                     static_cast<unsigned long long>(p99), static_cast<unsigned long long>(p999),
                     static_cast<unsigned long long>(mean), thr / 1e6,
                     static_cast<unsigned long long>(mn), static_cast<unsigned long long>(mx),
                     kOps, static_cast<unsigned long long>(book->trade_count()), book->resting(),
                     static_cast<unsigned long long>(bin_w), bin_csv,
                     static_cast<unsigned long long>(p50), static_cast<unsigned long long>(p90),
                     static_cast<unsigned long long>(p99), static_cast<unsigned long long>(p999),
                     static_cast<unsigned long long>(bin_w));
        std::fclose(hf);
        std::printf("  html        %s\n", html_path);
    }

    if (FILE* jf = std::fopen(json_path, "w")) {
        write_json(jf, kOps, p50, p90, p99, p999, mean, mn, mx, thr, bins, bin_w, cap,
                   book->trade_count(), book->resting());
        std::fclose(jf);
        std::printf("  json        %s\n", json_path);
    }

    ::operator delete(samples, sizeof(uint64_t) * kOps, std::align_val_t{64});
    delete book;
    return 0;
}
