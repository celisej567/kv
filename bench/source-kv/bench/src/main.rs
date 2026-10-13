// Benchmark backend for bench/compare: same commands and output keys as kv_compare.cpp, but for the
// Rust crate source-kv (SourceRS). Values are untyped there (everything is a string), so the
// lookups are GetString-like only.
use source_kv::{to_string, Deserializer, Value};
use std::alloc::{GlobalAlloc, Layout, System};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::time::Instant;

struct Counting;
static LIVE: AtomicUsize = AtomicUsize::new(0);
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 {
        LIVE.fetch_add(l.size(), Ordering::Relaxed);
        System.alloc(l)
    }
    unsafe fn dealloc(&self, p: *mut u8, l: Layout) {
        LIVE.fetch_sub(l.size(), Ordering::Relaxed);
        System.dealloc(p, l)
    }
    unsafe fn realloc(&self, p: *mut u8, l: Layout, n: usize) -> *mut u8 {
        LIVE.fetch_add(n, Ordering::Relaxed);
        LIVE.fetch_sub(l.size(), Ordering::Relaxed);
        System.realloc(p, l, n)
    }
}
#[global_allocator]
static A: Counting = Counting;

fn load(path: &str) -> Option<Value> {
    let s = std::fs::read_to_string(path).ok()?;
    match Deserializer::from_str(&s).parse_root() { Ok(v) => Some(v), Err(e) => { eprintln!("{e}"); None } }
}

fn ms(t: Instant) -> f64 {
    t.elapsed().as_secs_f64() * 1e3
}

fn status_kb(field: &str) -> usize {
    std::fs::read_to_string("/proc/self/status")
        .unwrap()
        .lines()
        .find(|l| l.starts_with(field))
        .and_then(|l| l[field.len()..].split_whitespace().next()?.parse().ok())
        .unwrap_or(0)
}

fn fsize(p: &str) -> u64 {
    std::fs::metadata(p).map(|m| m.len()).unwrap_or(0)
}

// the single root object's contents (the file has one root)
fn inner(v: &Value) -> &Value {
    &v.as_obj().unwrap()[0].1
}

fn lcg(s: &mut u64) -> u64 {
    *s = s.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
    *s >> 33
}

fn cmd_load(path: &str, reps: usize) -> i32 {
    let mb = fsize(path) as f64 / 1048576.0;
    let (mut l, mut d) = (1e300f64, 1e300f64);
    for _ in 0..reps {
        let t = Instant::now();
        let Some(kv) = load(path) else { eprintln!("load failed"); return 1 };
        l = l.min(ms(t));
        let t = Instant::now();
        drop(kv);
        d = d.min(ms(t));
    }
    println!("load_ms {l:.2}\nload_mbps {:.1}\ndestroy_ms {d:.2}", mb / (l / 1000.0));
    0
}

// parse only: the borrowed tape the newer crate builds before it makes owned values (no owned tree)
fn cmd_tape(path: &str, reps: usize) -> i32 {
    let mb = fsize(path) as f64 / 1048576.0;
    let mut l = 1e300f64;
    for _ in 0..reps {
        let t = Instant::now();
        let Ok(s) = std::fs::read_to_string(path) else { return 1 };
        if source_kv::from_str::<serde::de::IgnoredAny>(&s).is_err() { eprintln!("load failed"); return 1 }
        l = l.min(ms(t));
    }
    println!("load_ms {l:.2}\nload_mbps {:.1}", mb / (l / 1000.0));
    0
}

fn cmd_save(path: &str, out: &str, reps: usize) -> i32 {
    let Some(kv) = load(path) else { return 1 };
    let mut best = 1e300f64;
    for _ in 0..reps {
        let t = Instant::now();
        let s = to_string(&kv).unwrap();
        std::fs::write(out, s).unwrap();
        best = best.min(ms(t));
    }
    let mb = fsize(out) as f64 / 1048576.0;
    println!("save_ms {best:.2}\nsave_mbps {:.1}\nsaved_bytes {}", mb / (best / 1000.0), fsize(out));
    0
}

fn cmd_mem(path: &str) -> i32 {
    let heap0 = LIVE.load(Ordering::Relaxed);
    let rss0 = status_kb("VmRSS:");
    let Some(kv) = load(path) else { return 1 };
    let heap1 = LIVE.load(Ordering::Relaxed);
    let rss1 = status_kb("VmRSS:");
    let hwm = status_kb("VmHWM:");
    let file = fsize(path) as f64;
    println!("file_bytes {}", file as u64);
    println!("heap_bytes {}", heap1 - heap0);
    println!("heap_over_file {:.2}", (heap1 - heap0) as f64 / file);
    println!("rss_bytes {}", (rss1 - rss0) * 1024);
    println!("peak_rss_bytes {}", (hwm - rss0) * 1024);
    std::hint::black_box(&kv);
    0
}

fn cmd_lookup(path: &str, n: usize) -> i32 {
    let Some(kv) = load(path) else { return 1 };
    let root = inner(&kv);
    let (prefix, scalars) = if root.get("key_0").is_some() { ("key_", true) } else if root.get("item_0").is_some() { ("item_", false) } else {
        eprintln!("no key_/item_ keys in this file");
        return 1;
    };
    let count = root.as_obj().unwrap().len();
    let mut seed = 99u64;
    let names: Vec<String> = (0..n).map(|_| format!("{prefix}{}", lcg(&mut seed) % count as u64)).collect();
    let t = Instant::now();
    let mut sink = 0usize;
    for s in &names {
        sink += if scalars { root.get_string(s).map_or(0, |x| x.len()) } else { root.get(s).map_or(0, |x| x as *const _ as usize) };
    }
    println!("top_level_lookup_ns {:.1}\ntop_level_objects {count}", ms(t) * 1e6 / n as f64);

    let items: Vec<&Value> = (0..count.min(4096)).filter_map(|i| root.get(&format!("{prefix}{i}"))).collect();
    if !items.is_empty() && items[0].get_string("count").is_some() {
        let geti = |v: &Value| v.get_string("count").and_then(|s| s.parse::<i64>().ok()).unwrap_or(-1);
        let (mut sc, mut sn) = (0i64, 0i64);
        for it in &items {
            sc += geti(it);
            sn += it.get_string("name").map_or(0, |s| s.len()) as i64;
        }
        println!("checksum {sc} {sn} {}", items.len());
        let reps = 2_000_000usize;
        let t = Instant::now();
        for i in 0..reps {
            sink = sink.wrapping_add(geti(items[i % items.len()]) as usize);
        }
        println!("small_object_getint_ns {:.1}", ms(t) * 1e6 / reps as f64);
        let t = Instant::now();
        for i in 0..reps {
            sink += items[i % items.len()].get_string("name").map_or(0, |s| s.len());
        }
        println!("small_object_getstring_ns {:.1}", ms(t) * 1e6 / reps as f64);
    }
    std::hint::black_box(sink);
    0
}

fn cmd_mt(path: &str, threads: usize, reps: usize) -> i32 {
    drop(load(path));
    let mb = fsize(path) as f64 / 1048576.0;
    let ready = AtomicUsize::new(0);
    let go = AtomicBool::new(false);
    let failed = AtomicUsize::new(0);
    let start = std::thread::scope(|sc| {
        for _ in 0..threads {
            sc.spawn(|| {
                ready.fetch_add(1, Ordering::SeqCst);
                while !go.load(Ordering::SeqCst) {}
                for _ in 0..reps {
                    match load(path) {
                        Some(kv) => drop(kv),
                        None => { failed.fetch_add(1, Ordering::SeqCst); return; }
                    }
                }
            });
        }
        while ready.load(Ordering::SeqCst) != threads {}
        let t = Instant::now();
        go.store(true, Ordering::SeqCst);
        t
    });
    let m = ms(start);
    println!("threads {threads}\nmt_ms {m:.2}\nmt_total_mbps {:.1}\nfailed {}", mb * (threads * reps) as f64 / (m / 1000.0), failed.load(Ordering::SeqCst));
    0
}

fn main() {
    let a: Vec<String> = std::env::args().collect();
    let c = a.get(1).map(String::as_str).unwrap_or("");
    let code = match (c, a.len()) {
        ("load", 4) => cmd_load(&a[2], a[3].parse().unwrap()),
        ("tape", 4) => cmd_tape(&a[2], a[3].parse().unwrap()),
        ("save", 5) => cmd_save(&a[2], &a[3], a[4].parse().unwrap()),
        ("mem", 3) => cmd_mem(&a[2]),
        ("lookup", 4) => cmd_lookup(&a[2], a[3].parse().unwrap()),
        ("mt", 5) => cmd_mt(&a[2], a[3].parse().unwrap(), a[4].parse().unwrap()),
        _ => { eprintln!("usage: load|save|mem|lookup|mt ..."); 2 }
    };
    std::process::exit(code);
}
