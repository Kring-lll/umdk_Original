# qbuf_pool_tool 娴嬭瘯浣跨敤

## 1. 姒傝堪

`qbuf_pool_tool` 鏄?umq 澶氱骇 size_class qbuf pool 鐨勬枃浠堕┍鍔ㄦ祴璇曞伐鍏枫€備竴涓?`.txt` 鑴氭湰 = 涓€涓敤渚嬶紝涓荤嚎绋嬭鑴氭湰鍒嗗彂鍛戒护鍒板伐浣滅嚎绋嬫墽琛屻€傝仛鐒﹂獙璇佸绾ф睜鏍稿績鐗规€э細澶氱骇 sc 璺敱銆乀LS 瀛楄妭棰勭畻銆乫etch/return 鐢熷懡鍛ㄦ湡銆佸绾跨▼ TLS 闅旂銆佹硠婕忔娴嬨€?
## 2. 鏋勫缓涓庤繍琛?
鐙珛 cmake 椤圭洰锛屼笉渚濊禆椤跺眰 hcom cmake銆佷笉闇€瑕?`BUILD_TESTS=ON`銆佷笉闇€瑕佽 gtest/mockcpp銆備粠浠撳簱鏍癸細

```bash
cmake -S test/urpc/umq/qbuf_pool_tool -B build_qbuf_tool
cmake --build build_qbuf_tool -j
./build_qbuf_tool/qbuf_pool_tool <script.txt>
```

璺戞壒锛?```bash
for f in test/urpc/umq/qbuf_pool_tool/cases/*.txt; do
    echo "=== $(basename $f) ==="
    ./build_qbuf_tool/qbuf_pool_tool "$f"
done
```

閫€鍑虹爜锛?=鍏ㄩ儴鍛戒护鎴愬姛锛岄潪0=鏈夊懡浠ゅけ璐ャ€?
## 3. 鍛戒护闆?
姣忚涓€鍛戒护,`#` 寮€澶存敞閲?鍚?inline `#`),绌鸿蹇界暐銆?
### 3.1 鍗曡鍛戒护

| 鍛戒护 | 褰㈠紡 | 鎵ц鑰?| 璇存槑 |
|---|---|---|---|
| `init` | `init count=2 blockSizes=4K,64K mode=split scaleCap=on tlsBudget=0 poolMaxSz=0 expSlotSz=0 weights=2,1 escape=on expThreshold=0 tlsExpandBudget=0 threads=1` | 涓荤嚎绋?| key=value 椤哄簭鏃犲叧,缂虹渷鐢ㄩ粯璁?`threads=N` 鍚姩 N 宸ヤ綔绾跨▼銆?*涓嬮檺:threads鈮?**(鏃犱笂闄?浣?threads>8 鏃?spinlock 鑷棆鐑?CPU,瑙?case_13)銆俙blockSizes=` 蹇呭～(鏃?`mult` 鍑犱綍绾ф暟鍥為€€),`weights=` 缂虹渷鏃剁敓浜т唬鐮佸厹搴?`{1,1}` 绛夋潈 |
| `thread_K alloc` | `thread_K alloc <size> [num=1] [headroom=0]` | worker K | 鍒嗛厤,size 鏀寔 `4K`/`64K`/`1M` 鍚庣紑;杈撳嚭姣忎釜 buf 鐨?sc/ptr/buf_size銆?*涓婇檺:num鈮?000** |
| `thread_K free` | `thread_K free <orig_idx>` 鎴?`thread_K free all` | worker K | `orig_idx` 鏄?buf 鐨?*绋冲畾绱㈠紩**(alloc 鏃跺垎閰?鍗曡皟閫掑,erase 涓嶅墠绉?;`free all` 娓呰绾跨▼鍏ㄩ儴骞堕噸缃?orig_idx 璁℃暟鍣?|
| `info` | `info` | 涓荤嚎绋?| 鍏ㄩ噺鐘舵€?寮€鍙戣皟璇曠敤,~50 琛? |
| `status` | `status` | 涓荤嚎绋?| 绮剧畝鐘舵€?娴嬭瘯鐢?3 娈典竴涓) |
| `quit` | `quit` | 涓荤嚎绋?| 鎻愬墠閫€鍑?瑙﹀彂 Cleanup:娉勬紡妫€鏌?+ join workers) |

**init 鍙傛暟瀵圭収琛?瀵瑰簲 cfg 瀛楁)**:

| 鍙傛暟 | cfg 瀛楁 | 榛樿鍊?| 璇箟 |
|---|---|---|---|
| `poolMaxSz` | `umq_buf_pool_max_size` | 2GB(=0 鏃? | **鎬诲唴瀛?ceiling** = 鍒濆姹?+ 鍏ㄩ儴 expansion 妲姐€傜‖涓婇檺 6GB(`scaleCap=on` 鏃?`poolMaxSz>6G` 浼氳 umq_qbuf_pool_init 鎷掔粷) |
| `expSlotSz` | `expansion_size` | 32MB(=0 鏃? | **per-slot 鐩爣鍐呭瓨**(姣忔 expansion 鎯?鍔?澶氬皯)銆傜粡閲忓寲(梅blk_size鈫捗穝ub_slot_blk_count鈫捗?MB)鍚庡彉鎴愬疄闄?`slot.total_buf_size`,涓?`expSlotSz` 涓嶄弗鏍肩浉绛?|
| `tlsBudget` | `tls_pool_mem_budget` | 96MB(=0 鏃? | 鍏ㄥ眬 TLS bytes cap |
| `blockSizes` | `explicit_block_sizes[]` | 鏈缃啋mult 鍑犱綍绾ф暟鍥為€€ | **鏄惧紡 per-SC block_size 鏁扮粍**(commit 600d24a 鍚庢浛浠?`size_class_step_multiplier`)銆傛牸寮?`blockSizes=4K,64K` 鎴?`4K,32K,128K`,閫楀彿鍒嗛殧,token 鏁板繀椤荤瓑浜?`count`銆傜敓浜ц矾寰勮 `umq_backend.cpp:67-69` memcpy `UmqSetting::UMQ_EXPLICIT_BLOCK_SIZES = {4096, 65536}`銆?*鏈缃椂**宸ュ叿鐢?`mult` 璁＄畻鍑犱綍绾ф暟 `base * mult^i`(榛樿 `mult=16 base=4K` 鈫?`{4K, 64K}` 宸у悎涓庣敓浜т竴鑷?,浣嗗彧鑳借〃杈惧嚑浣曠骇鏁扳€斺€旈潪鍑犱綍鍒嗗竷(濡?`{4K, 32K, 128K}`)蹇呴』鐢?`blockSizes=`銆倀oken 鏀寔 K/M/G 鍚庣紑(`ParseSize`) |
| `weights` | `per_sc_weights[]` | 鏈缃啋鍏?0鈫掔敓浜у厹搴?{1,1} | **per-SC 鍒嗛厤鏉冮噸鏁扮粍**(commit 600d24a 鍚庢浛浠?`lazy_init_block_size_threshold`)銆傛牸寮?`weights=2,1` 鎴?`1,1,0`,閫楀彿鍒嗛殧,token 鏁板繀椤荤瓑浜?`count`銆?*鐗规畩鍊?0 = lazy SC**(init 鏃朵笉棰勫垎閰?棣栨 alloc 瑙﹀彂 expansion,鏇夸唬鏃х殑 `lazyThreshold` 闃堝€兼満鍒?鏀逛负 per-SC 绮掑害鎺у埗)銆傜敓浜ц矾寰勮 `umq_backend.cpp:70-72` memcpy `UmqSetting::UMQ_PER_SC_WEIGHTS = {2, 1}`(4K 鎷?2x 浠介閬垮厤 expansion)銆?*鏈缃椂** `cfg.per_sc_weights[]` 鍏?0,鐢熶骇浠ｇ爜 `umq_qbuf_pool.c:1595-1597` 妫€娴嬪叏 0 鈫?鍥為€€榛樿 `{1, 1}` 绛夋潈 |
| `escape` | `disable_malloc_escape`(鍙栧弽) | on(=鍚敤 escape) | **malloc escape 寮€鍏?*銆俙escape=off` 鏃?`cfg.disable_malloc_escape=true`,escape 璺緞鍏抽棴:`umq_qbuf_escape_alloc` 绔嬪嵆杩斿洖 EINVAL(`umq_qbuf_pool.c:2034`)銆乣umq_normal_qbuf_alloc` 璺宠繃鍐呴儴 escape 鍒嗘敮(`:2143`/`:2182`)銆傜敤浜庢祴璇?pool 鑰楀敖鏃惰繑鍥?ENOMEM 鑰岄潪 fallback 鍒?malloc escape 鐨勫満鏅?|
| `expThreshold` | `expansion_threshold` | 0(鈫掔敓浜ч粯璁?30) | **expansion 瑙﹀彂姘翠綅鐧惧垎姣?*銆傛帶鍒?`trigger_expand_block_num = expansion_block_count * threshold / 100`銆? 鏃剁敓浜т唬鐮佸彇 `QBUF_POOL_DEFAULT_EXPANSION_THRESHOLD`(=30%)銆傝寖鍥?**[1, 100]**,瓒婄晫 init 杩斿洖 EINVAL(`umq_qbuf_pool.c:1225-1228`)銆傞珮鍊?濡?90)= expansion 鏇存棭瑙﹀彂;浣庡€?濡?5)= 婊炲悗瑙﹀彂 |
| `tlsExpandBudget` | `tls_expand_mem_budget` | 0(鈫掔敓浜ч粯璁?7/8 tlsBudget) | **per-thread TLS bytes cap**銆? 鏃剁敓浜т唬鐮佸彇 `umq_qbuf_pool_expand_max(tls_pool_mem_budget)` = 7/8 鐨?`tlsBudget`(`umq_qbuf_pool.c:1214-1216`)銆傛帶鍒?TLS fetch 鎴柇:`local_total_bytes + delta > tlsExpandBudget` 鏃舵埅鏂?delta(`:1666-1669`)銆備笌 `tlsBudget`(鍏ㄥ眬鎵€鏈夌嚎绋嬫€?cap)鐙珛璇箟 |
| `count`/`mode`/`scaleCap`/`threads` | (瀵瑰簲瀛楁) | 瑙佺ず渚?| `count` 鏄?SC 鏁伴噺(1..UMQ_SIZE_CLASS_MAX,榛樿 2);**`mult` 鍜?`base` 鍙傛暟宸茬Щ闄?*鈥斺€擿base` 浠?`blockSizes[0]` 鑷姩鎺ㄥ(缁?`BlkSizeToEnum` 鍙嶅悜鏄犲皠) |

**`blockSizes=` 鍘嗗彶**(commit 600d24a 鍚?:
- 鏃у瓧娈?`size_class_step_multiplier`(鍗曚竴鏁存暟,鍑犱綍绾ф暟 `base * mult^i`)宸蹭粠 `qbuf_pool_cfg_t` 绉婚櫎,鏀逛负鏄惧紡鏁扮粍 `explicit_block_sizes[]`
- 宸ュ叿鍘?`mult` 鍙傛暟(鍑犱綍绾ф暟鍥為€€)鍜?`base` 鍙傛暟(鏄惧紡鏋氫妇)宸?*褰诲簳绉婚櫎**鈥斺€擿base` 鐜颁粠 `blockSizes[0]` 鑷姩鎺ㄥ(缁?`utils.cpp` 鐨?`BlkSizeToEnum()` 鍙嶅悜鏄犲皠,4096鈫抈BLOCK_SIZE_4K` 绛?,閬垮厤鍐椾綑鍙傛暟
- `blockSizes=` 鐜颁负**蹇呭～**鍙傛暟(鏃犲洖閫€);token 鏀寔 K/M/G 鍚庣紑(`ParseSize`),濡?`4K,64K` 鎴?`4096,65536`

**`weights=` 涓庢棫 `lazyThreshold` 鐨勫叧绯?*(commit 600d24a 鍚?:
- 鏃у瓧娈?`lazy_init_block_size_threshold`(鍗曚竴闃堝€?blk_size 鈮?闃堝€肩殑 SC 璧?lazy)宸茶绉婚櫎,鏀逛负 per-SC 鏁扮粍 `per_sc_weights[]`
- `weights[i]=0` 鍗宠 SC lazy(璇箟涓庢棫 `lazyThreshold` 绛変环,浣嗙矑搴︿粠"闃堝€?鏀逛负"per-SC")
- 鏃х敤渚?`case_16~20` 宸蹭粠 `lazyThreshold=1M` 杩佺Щ鍒?`weights=1,1,0`(3 SCs,sc2 鍗?1M lazy)

**`poolMaxSz` 涓?`expSlotSz` 鐨勫叧閿尯鍒?*(鏄撴贩):
- `poolMaxSz` 鏄?*鎬讳笂闄?*,缁忔淳鐢?`expansion_mem_size_max = poolMaxSz - 鍒濆姹犲ぇ灏廯 鍚?鐢?CAS 闄愬埗 `exp_total_mem_pool_size`(褰撳墠宸茬敤 expansion 鍐呭瓨)銆?*杩欐墠鏄湡姝?闄愬埗鎬诲唴瀛?鐨勯厤缃?*銆?- `expSlotSz` 鏄?*per-slot 鐩爣**,缁忔淳鐢熼摼鎺ㄧ畻 per-slot 瀹為檯鍒嗛厤澶у皬,init 鏃朵笉涓?`poolMaxSz` 鍋氭瘮杈冦€傝 `expSlotSz > poolMaxSz` 浼氳 init 闈欓粯閫氳繃銆佽繍琛屾椂 expansion 蹇呰触(姣忔 slot 鐢宠閮借秴 cap)銆?- 楠岃瘉 expansion 琛屼负鏃?**鍏堣皟 `poolMaxSz` 璁惧皬**(濡?`poolMaxSz=64M`)璁?expansion 瑙﹀彂鍚庤兘寰堝揩鎾炲埌涓婇檺;鍐嶇敤 `expSlotSz` 鎺у埗 per-slot 绮掑害銆?
### 3.2 骞惰鍧?`parallel` / `join`

```
parallel
  thread_0 alloc 4096 100
  thread_1 alloc 4096 100
  thread_2 alloc 4096 100
join
```

**璇箟**:`parallel` 鍜?`join` 涔嬮棿鐨勬墍鏈?`thread_K` 鍛戒护寮傛娲惧彂(涓荤嚎绋嬩笉闃诲绛夊緟姣忎釜 worker),`join` 鏃剁粺涓€鏀跺壊銆傚涓?worker 鐪熸骞跺彂鎵ц鈥斺€旈獙璇佺敓浜т唬鐮佺殑 spinlock(`block_pool[sc].global_mutex`)銆佸師瀛愯鏁板櫒(`g_total_local_cap_with_data_bytes`)銆丆AS(`is_async_expanding`)绛夊苟鍙戝師璇€?
**纭鍒?*:
1. 鍧楀唴鍙兘鏀?`thread_K alloc/free` 鍛戒护,绂佹 `init/info/status/quit/repeat/parallel`
2. 鍚屼竴 worker 鍦ㄥ悓涓€ `parallel` 鍧楀唴涓嶈兘鏃?alloc 鍙?free(閬垮厤 `w.alloced` 涓荤嚎绋?drain 涓?worker free 鐨?race)銆傞渶瑕?alloc+free 鏃剁敤涓や釜鐙珛 parallel 鍧?3. 鍚屼竴 worker 涓嶈兘鍦ㄥ悓涓€ parallel 鍧楀唴鍑虹幇涓ゆ(鐢ㄥ崟鏉?`alloc num=N` 浠ｆ浛)
4. `parallel` 涓嶈兘宓屽 `parallel`
5. `info`/`status` 蹇呴』鍦?`join` 涔嬪悗璋冪敤(姝ゆ椂鎵€鏈?worker 宸插仠,DFX 鎷垮埌绋冲畾蹇収)

### 3.3 閲嶅鍧?`repeat N` / `end`

```
repeat 1000
  parallel
    thread_0 alloc 4096 10
    thread_0 free all
  join
end
```

**璇箟**:`repeat N` 鍜?`end` 涔嬮棿鐨勫唴瀹逛覆琛屾墽琛?N 娆°€?*涓婇檺:N鈮?0000**銆傛瘡娆¤凯浠ｅ彲浠ユ槸鍗曡 `thread_K` 鍛戒护(涓茶)鎴栦竴涓祵濂楃殑 `parallel/join` 鍧椼€?
**纭鍒?*:
1. 鍧楀唴绂佹 `info/status/init/quit`(妫€鏌ュ畧鎭掑湪 repeat 涔嬪悗鍋?
2. 鍧楀唴绂佹宓屽 `repeat` 鎴?`stress_repeat`
3. 鍧楀唴鏈€澶氫竴涓?`parallel` 瀛愬潡
4. 鍗曟 race 姒傜巼浣?鐢?`repeat` 鏀惧ぇ绔炴€佺獥鍙?5. 鍧楀唴鍏佽 `snapshot N` 鎸囦护(瑙?搂3.5)

### 3.4 闀跨ǔ閲嶅鍧?`stress_repeat` / `end`(鏃犻檺寰幆)

```
stress_repeat
  parallel
    thread_0 alloc 4096 10
    thread_1 alloc 4096 10
  join
  parallel
    thread_0 free all
    thread_1 free all
  join
  snapshot 1000
end
```

**璇箟**:`stress_repeat` 鍜?`end` 涔嬮棿鐨勫唴瀹?*鏃犻檺寰幆鎵ц**,鐩村埌:
1. **SIGINT/SIGTERM**(Ctrl-C 鎴?`kill -INT <pid>`):浼橀泤閫€鍑?鈥?褰撳墠 iter 瀹屾垚鍚?break,鎵撳嵃 `[stress] interrupted by signal at iter N` + 鏈€缁?summary,parser 缁х画涓嬩竴琛?鍙悗缁?`status`/`quit`)銆傞€€鍑虹爜 0
2. **ExecuteBlockLines 杩斿洖闈?0**(alloc/free 澶辫触):绔嬪嵆 abort,閫€鍑虹爜 1
3. **绗簩娆?Ctrl-C**:`OnStopSignal` 閲嶈榛樿 handler 骞?raise,杩涚▼绔嬪嵆姝?鐢ㄤ簬 cleanup 鍗℃鍦烘櫙)

**涓?`repeat` 鐨勫叧閿尯鍒?*(memory-bounded 璁捐):
1. **涓嶈褰?action 鍘嗗彶**:`g_actions` 鍦?stress 妯″紡涓嬩笉 push 浠讳綍鏉＄洰,鏃犻檺寰幆鍚?`g_actions` 澶у皬鎭掔瓑浜?stress_repeat 涔嬪墠鐨勬潯鐩暟(閫氬父鍙湁 1 鏉?`init`)銆俙DrainWorkerResult` 鍦?`g_stress_mode==true` 鏃惰烦杩囨墍鏈?`g_actions` 鍐欏叆
2. **涓嶆墦鍗?per-buf 璇︽儏**:`DrainWorkerResult` 鍦?stress 妯″紡涓嬭烦杩?`printf("[tN] buf[i]...")`,閬垮厤鏃犻檺杩唬 脳 1000 buf 杈撳嚭娲按
3. **姣忚疆鏈熬娓呯┖ `Worker::alloc_ops` 骞堕噸缃?`next_orig_idx`**:褰撴煇 worker 鐨?`alloced.empty()`(鐢ㄦ埛淇濊瘉 alloc/free 閰嶅)鏃?`DoStressRepeatBlock` 闈欓粯 `alloc_ops.clear()` + `next_orig_idx = 0`銆傝繖瑕嗙洊 `free <idx>` 妯″紡(涓嶅儚 `free all` 鑷姩閲嶇疆)鈥斺€旇 `free 0`/`free 1` 绛夌ǔ瀹氱储寮曞湪鏃犻檺寰幆涓嬫瘡杞兘鑳芥纭尮閰?鍚﹀垯 `next_orig_idx` 鎸佺画澧為暱,绗簩杞捣 `free 0` 鎵句笉鍒?orig_idx=0 鐨?buf)銆?*鐢ㄦ埛蹇呴』淇濊瘉姣忚疆 alloc 鍜?free 閰嶅**,鍚﹀垯 `alloced` 闈炵┖鏃?`alloc_ops` 鍜?`next_orig_idx` 閮戒笉浼氶噸缃?淇濇姢 `FREE_IDX` cross-ref),鍐呭瓨浼氶殢娉勬紡绾挎€у闀?鏈€缁堣Е鍙?`WORKER_ALLOCED_CAP=65536` 涓婇檺缁堟 stress
4. **StressCounters 姹囨€?*:`g_stress` 鍏ㄥ眬璁℃暟鍣?total_allocs / total_frees / total_bufs_alloced / total_bufs_freed / errors)鍦?`DrainWorkerResult` + `DoFree` serial 璺緞 bump,涓荤嚎绋嬪崟绾跨▼鏃犻渶 atomic銆俙status` 鍦?`g_stress_mode==true` 鎴?`g_stress.iters_done>0` 鏃跺湪椤堕儴杩藉姞 `stress running` / `stress interrupted` / `stress done` 娈?5. **鏃犻檺寰幆 鈮?鏃犵晫鍐呭瓨**:宸ュ叿鑷韩鍐呭瓨鎭掑畾(g_actions 涓嶅闀裤€乤lloc_ops 姣忚疆娓呯┖銆乧ounters 1 涓?struct銆乻napshot 鐩磋緭 stdout 涓嶅瓨)銆傚彲鏀惧績璺戝ぉ绾у埆闀跨ǔ

**纭鍒?*(鍚?`repeat`):
1. 鍧楀唴绂佹 `info/status/init/quit`
2. 鍧楀唴绂佹宓屽 `repeat` 鎴?`stress_repeat`
3. 鍧楀唴鍏佽 `parallel/join` 瀛愬潡鍜?`snapshot N` 鎸囦护
4. **鐢ㄦ埛淇濊瘉姣忚疆 alloc/free 閰嶅**(宸ュ叿涓嶅己鍒跺畧鎭掓鏌?浣嗘硠婕忔椂鍐呭瓨浼氬闀?鏃犻檺寰幆涓嬪繀鐒?OOM)
5. **涓嶈兘鍦?batch 璺戞壒涓贩鐢?* 鈥?stress_repeat 鏃犻檺寰幆,鑷姩鍖栬剼鏈細鍗′綇銆傛墜鍔?Ctrl-C 鎺у埗

**Ctrl-C 閫€鍑哄悗鏌ヨ**:
```bash
# Terminal 1: 鍚姩 stress_repeat
./build_qbuf_tool/qbuf_pool_tool cases/case_21_stress_repeat.txt

# (璺戜竴娈垫椂闂村悗鎸?Ctrl-C)
# 杈撳嚭:
# [stress] interrupted by signal at iter 12345 (waiting for current iter to complete, then exiting gracefully)
# [stress interrupted] iters=12345 uptime=0h 1m 2s allocs=98760 frees=98760 ...
# ====================[ status ]====================
# stress interrupted:
#   iters: 12345
#   ...
```

### 3.5 snapshot 鎸囦护(鍙€?鐢ㄤ簬 `repeat` 鍜?`stress_repeat` 鍧楀唴)

```
stress_repeat 1000000
  parallel
    thread_0 alloc 4096 10
  join
  parallel
    thread_0 free all
  join
  snapshot 1000
end
```

**璇箟**:姣?N 杞凯浠ｆ墦鍗颁竴琛岃繘搴︽憳瑕佸埌 stdout,**涓嶅瓨鍏ヤ换浣?vector**鈥斺€斿唴瀛樻亽瀹?涓庤凯浠ｆ暟鏃犲叧銆?
杈撳嚭鏍煎紡(stress 妯″紡):
```
[stress] iter=1000/1000000 uptime=0h 12m 34s allocs=8000 frees=8000 errors=0 alloced_held=0
```

`repeat`(闈?stress)妯″紡涓嬭緭鍑?`[snapshot]` 鍓嶇紑,瀛楁鐩稿悓銆?
**瀹炵幇**:`DoRepeatBlock` / `DoStressRepeatBlock` 棰勬壂鎻忓潡鍐呯殑 `snapshot N` 鎸囦护鎻愬彇 interval,鍦ㄥ惊鐜腑 `(i+1) % interval == 0` 鏃舵墦鍗般€俙ExecuteBlockLines` 鎶?`snapshot` 褰?no-op 璺宠繃銆?
### 3.6 actions 搴忓彿璇箟

`g_actions` 搴忓彿鎸?*娲惧彂椤哄簭**璁板綍(涓嶆寜瀹屾垚椤哄簭)銆傚苟琛屽潡鍐呯殑鍛戒护鍦ㄦ淳鍙戞椂棰勫崰 `g_actions` 妲戒綅,`SyncAll` 鏀跺壊鏃跺洖濉瓧绗︿覆銆傝繖淇濊瘉搴忓彿绋冲畾鍙鐜扳€斺€斿悓鏍风殑鑴氭湰澶氭璺戝嚭鏉ョ殑搴忓彿涓€鑷?渚夸簬 debug銆?

## 4. 鐢ㄤ緥璇存槑锛坈ases/锛?
### 4.1 涓茶鐢ㄤ緥(鍘?case_01~07)

| 鐢ㄤ緥 | 鍦烘櫙 | 楠岃瘉鐐?|
|---|---|---|
| `case_01_basic_init_alloc_free.txt` | init + info 鍏ㄩ噺 + 璺?sc alloc + free all | info 澶氱骇瀛楁 + 璺敱鍑芥暟 + TLS 瀛楄妭棰勭畻 |
| `case_02_status.txt` | status 鍩烘湰娴佺▼锛坕nit鈫抋lloc鈫抐ree all鈫抯tatus锛?| actions / held allocs / pool state 婕斿彉 |
| `case_03_leak.txt` | alloc 涓?free | alloced > 0 + 閫€鍑?NOTE per thread |
| `case_04_interleaved_alloc_free.txt` | 鍗曠嚎绋嬬┛鎻?alloc/free锛屾瘡姝?status | TLS 澶嶇敤 + 鍏ㄧ▼瀹堟亽 + idx 璇箟 |
| `case_05_multi_thread.txt` | 澶氱嚎绋嬪熀纭€锛坱hreads=2锛?| per-thread TLS 闅旂锛坱0/t1 涓嶅悓 tid/fetch锛?|
| `case_06_complex_multi_thread.txt` | 澶嶆潅澶氱嚎绋嬶紙threads=3锛屾贩鍚?sc锛岀┛鎻?free锛屾硠婕忥級 | per-thread TLS + held allocs per-worker + per-thread 娉勬紡 |
| `case_07_tls_fetch_return.txt` | TLS fetch/return 鐢熷懡鍛ㄦ湡锛堝崟绾跨▼锛?| fetch_from_global 鈫?free 杩?TLS 鈫?self_shrink return_to_global |

### 4.2 骞惰鐢ㄤ緥(鏂板 case_08~13)

| 鐢ㄤ緥 | 鍦烘櫙 | 楠岃瘉鐐?| 娑夊強鍘熻 |
|---|---|---|---|
| `case_08_concurrent_alloc_race.txt` | 8 worker 鍚屾椂 alloc 鍚?sc(C1) | fetch_from_global spinlock race + 瀹堟亽 | `block_pool[sc].global_mutex` |
| `case_09_concurrent_free_race.txt` | 8 worker TLS 瑁呮弧鍚庡悓鏃?free all(C2) | return_to_global/TLS-update race + 瀹堟亽 | 鍚屼笂 |
| `case_10_mixed_alloc_free_race.txt` | repeat 500,姣?iter alloc 鍧?+ free 鍧?C3+C8) | alloc/free churn 瀹堟亽 + self_shrink 鍙嶅瑙﹀彂 | spinlock + atomic |
| `case_11_cross_sc_parallel.txt` | 涓嶅悓 worker 涓嶅悓 sc 骞跺彂(C7) | per-sc 閿佺矑搴﹂殧绂?+ 澶氱骇 sc 璺敱 | 涓嶅悓 spinlock |
| `case_12_stress_repeat.txt` | repeat 10000 闀胯窇(C8) | 闀跨▼瀹堟亽 + 璁℃暟鍣ㄧ疮绉?+ 鑷棆鍘嬪姏 | 缁煎悎鎵€鏈夊師璇?|
| `case_13_threads_beyond_8.txt` | threads=16(绐佺牬鏃?8 涓婇檺) | init 涓嶆姤閿?+ 16 worker 鍏ㄩ儴鎷?buf + 瀹堟亽 + per-thread TLS 16 琛屼笉鍚?tid | 缁煎悎鎵€鏈夊師璇?|

**娉ㄦ剰**:骞惰鐢ㄤ緥 threads=8 鏃?CPU 鍗犵敤楂?spinklock 鑷棆鐑?CPU),鍗曟 case_12 璺?1~3 绉掋€倀hreads>8(濡?case_13=16)鑷棆鍘嬪姏鏇村ぇ,鏈哄櫒鏍告暟涓嶈冻鏃跺疄闄呭苟鍙戝害鍙楅檺,浣?init 涓嶅啀鎷掔粷銆佸畧鎭掍粛鎴愮珛銆?
### 4.3 瀛楁鏄犲皠鐢ㄤ緥(鏂板 case_14~16)

淇 `expSz 鈫?cfg.expansion_size` 璇敤鍚庢柊澧炵殑 3 涓敤渚?楠岃瘉 `poolMaxSz`/`expSlotSz` 涓や釜瀛楁鏄犲皠鐨勬纭€с€?
| 鐢ㄤ緥 | 鍦烘櫙 | 楠岃瘉鐐?| 閫€鍑虹爜 |
|---|---|---|---|
| `case_14_poolMaxSz_field_mapping.txt` | `poolMaxSz=6G`(璧扮‖涓婇檺杈圭晫)姝ｅ悜 | 瀛楁琚纭В鏋愩€佷紶缁?`cfg.umq_buf_pool_max_size`銆佽 `umq_qbuf_pool_init` 鎺ュ彈;`info` 娈? `expansion_mem_size_max` 瀛楁鏄剧ず瀵瑰簲鍊?| 0 |
| `case_15_expSlotSz_field_mapping.txt` | `expSlotSz=64M` 姝ｅ悜 | 瀛楁琚纭В鏋愩€佷紶缁?`cfg.expansion_size`銆佽鎺ュ彈;`info` 娈? `expansion_size` 瀛楁鏄剧ず瀵瑰簲鍊?| 0 |
| `case_16_field_mapping_rejected.txt` | 鏃у悕 `expSz=` + `poolMaxSz=7G` 瓒婄晫 | (1) 鏃у悕 `expSz` 琚伐鍏?parsing 鎷掔粷(闃插洖褰? (2) `poolMaxSz>6G` 琚敓浜т唬鐮?`umq_qbuf_pool.c:1356` 鎷掔粷(璇佹槑鏄犲皠璧扮殑鏄?`umq_buf_pool_max_size` 璺緞鑰岄潪 `expansion_size`) | 1(棰勬湡澶辫触) |

**case_16 娉ㄦ剰**:涓や釜 init 閮介鏈熷け璐?閫€鍑虹爜=1 鏄纭涓恒€傜敤渚嬬殑"閫氳繃"璇箟鏄?涓や釜 init 閮芥寜棰勬湡琚嫆缁?,涓嶆槸"鎵€鏈夊懡浠ゆ垚鍔?銆俠atch 璺?`for f in cases/*.txt; do ... done`)浼氱湅鍒?case_16 閫€鍑虹爜闈?,灞為鏈熴€?
### 4.4 闀跨ǔ鐢ㄤ緥(鏂板 case_21~23)

**娉ㄦ剰**:case_21~23 鐢?`stress_repeat`(鏃犻檺寰幆),**涓嶈兘鍦?batch `for f in cases/*.txt; do ...` 涓窇** 鈥?浼氬崱浣忋€傛墜鍔ㄥ惎鍔?+ Ctrl-C 鎺у埗銆?
| 鐢ㄤ緥 | 鍦烘櫙 | 楠岃瘉鐐?|
|------|------|--------|
| `case_21_stress_repeat.txt` | 8 worker 鏃犻檺 `stress_repeat` + `snapshot 100` | g_actions 涓嶅闀?鍙?17 鏉?bootstrap);Ctrl-C 鍚?stress summary 娈垫樉绀?allocs=frees/errors=0;鐘舵€?interrupted |
| `case_22_stress_infinite.txt` | 8 worker 鏃犻檺 `stress_repeat` + `snapshot 10000` | 闀跨ǔ鍘嬪姏:RSS 鎭掑畾 ~204MB(=200MB pool + 4MB 宸ュ叿);閰嶅悎澶栭儴 `ps -o rss= -p <pid>` 鐩戞帶 |
| `case_23_stress_free_idx.txt` | 2 worker 鏃犻檺 `stress_repeat` 鐢?`free <idx>` 鑰岄潪 `free all` | 楠岃瘉 `alloc_ops` 鍜?`next_orig_idx` 鍦?iter 杈圭晫琚潤榛樻竻绌?閲嶇疆(鍚﹀垯 `free <idx>` 妯″紡涓?`next_orig_idx` 鎸佺画澧為暱,绗簩杞捣 `free 0` 鎵句笉鍒板尮閰?buf,鏃犻檺寰幆涓嬬珛鍗冲け璐? |

**鎵嬪姩楠岃瘉娴佺▼**:
```bash
# Terminal 1: 鍚姩 stress
./build_qbuf_tool/qbuf_pool_tool cases/case_22_stress_infinite.txt &
PID=$!

# Terminal 2: 鐩戞帶 RSS(搴斾繚鎸?~204MB 鎭掑畾)
while kill -0 $PID 2>/dev/null; do
    ps -o rss= -p $PID | awk '{print strftime("%H:%M:%S"), $1"KB"}'
    sleep 60
done

# Terminal 1(鍥炲埌 1): Ctrl-C 浼橀泤鍋滄
# 鏈熸湜杈撳嚭:
#   [stress] interrupted by signal at iter N (waiting for current iter to complete, then exiting gracefully)
#   [stress interrupted] iters=N uptime=... allocs=... frees=... errors=0 ...
#   ====================[ status ]====================
#   stress interrupted:
#     iters: N
#     ...
```

### 4.5 burst/sustained 鐢ㄤ緥(鏂板 case_14_burst / case_15_sustained)

> **缂栧彿璇存槑**:杩欎袱涓敤渚嬩笌 搂4.3 鐨?`case_14_poolMaxSz_field_mapping` / `case_15_expSlotSz_field_mapping` 鍏辩敤缂栧彿(case_14銆乧ase_15)銆俠atch 璺戞壒鏃跺悓鍙风敤渚嬮兘浼氭墽琛?璇箟涓婃槗娣锋穯鈥斺€斿缓璁湭鏉ラ噸缂栧彿(瑙?搂10 宸茬煡闂)銆?
楠岃瘉 high-water mark cap 淇:TLS cap 涓嶉殢 burst 鑶ㄨ儉銆乪xpansion pool 鍦?free 鍚庤兘鏀剁缉銆?
| 鐢ㄤ緥 | 鍦烘櫙 | 楠岃瘉鐐?| 閫€鍑虹爜 |
|---|---|---|---|
| `case_14_burst_alloc_free_shrink.txt` | 10000 sc1 buf burst(10 鎵姑?000,鍏?655360000 瀛楄妭)+ free all + 5 娆?status + 鏈€缁?info | 淇鍓?TLS sc1 buf=62K(inflated)銆乣expansion_count=15`(涓嶆敹缂?;淇鍚?<10K銆乣expansion_count<=2`(澶ч儴鍒嗘敹缂?銆?*娉ㄦ剰**:宸ュ叿闄愬埗鍗曟 alloc num鈮?000,鏁?10000 鎷?10 鎵?| 0 |
| `case_15_sustained_burst_stability.txt` | `repeat 1000` 杞?burst alloc 1000 sc1 + free all + 鏈€缁?status/info | 淇鍓?expansion memory 鍗曡皟澧為暱(姣忚疆 burst 鐣?residual);淇鍚庣ǔ瀹?cap 涓嶈啫鑳€銆乻lot 鏀剁缉) | 0 |

### 4.6 lazy init 鐢ㄤ緥(case_16~20)

> **缂栧彿璇存槑**:`case_16_lazy_init_3sc` 涓?搂4.3 鐨?`case_16_field_mapping_rejected` 鍏辩敤缂栧彿 case_16;`case_17_lazy_init_multi_expand` 涓?搂4.7 鐨?`case_17_without_data` 鍏辩敤缂栧彿 case_17銆俠atch 璺戞壒鏃跺悓鍙风敤渚嬮兘浼氭墽琛屸€斺€斿缓璁湭鏉ラ噸缂栧彿(瑙?搂10 宸茬煡闂)銆?
楠岃瘉 `weights=` 鍙傛暟涓?weight=0 鐨?lazy SC 琛屼负:weight=0 鐨?SC 鍦?init 鏃朵笉棰勫垎閰?棣栨 alloc 瑙﹀彂 expansion銆?
> **鍘嗗彶**:杩?5 涓敤渚嬪師浣跨敤 `lazyThreshold=1M`(鍩轰簬 `lazy_init_block_size_threshold` 闃堝€煎瓧娈?,commit `600d24a` 绉婚櫎璇ュ瓧娈垫敼涓?per-SC `per_sc_weights[]` 鏁扮粍鍚?鐢ㄤ緥杩佺Щ鍒?`weights=1,1,0`(sc2 鍗?1M SC weight=0 = lazy)銆傚悓鏃舵妸 `mult=16 base=4K` 鏀逛负鏇存樉寮忕殑 `blockSizes=4K,64K,1M`,骞朵慨澶?case_17/19 涓鐢ㄧ殑 `expSz=` 鏃у悕(搴?`expSlotSz=`)銆?
| 鐢ㄤ緥 | 鍦烘櫙 | 楠岃瘉鐐?|
|---|---|---|
| `case_16_lazy_init_3sc.txt` | 3-SC `blockSizes=4K,64K,1M` `weights=1,1,0`,鍩虹 alloc/free | 1M SC init 鏃朵负绌?`info` 娈? `glbl_free=0`);棣栨 alloc 1M 瑙﹀彂 expansion;3 涓?SC alloc + free all 姝ｅ父 |
| `case_17_lazy_init_multi_expand.txt` | lazy 1M 澶氳疆 expansion,`expSlotSz=2M`(2 鍧?杞?,4 涓?1M alloc | 瑙﹀彂 2 杞?expansion(姣忚疆 2 鍧?;`info` 娈? `exp_count`/`exp_total_blk` 鍙嶆槧澶氳疆绱Н |
| `case_18_lazy_init_combine.txt` | lazy 1M + COMBINE 妯″紡(`mode=combine`) | lazy init 涓?COMBINE 妯″紡鍙犲姞姝ｇ‘:buf_size = block_size(鏃?header 鍒嗙);1M SC 鍚屾牱璧?lazy 璺緞 |
| `case_19_lazy_init_concurrent.txt` | 8 worker 骞跺彂璺?SC alloc(t0-3 alloc 4K銆乼4-5 alloc 64K銆乼6-7 alloc 1M),`expSlotSz=4M` | per-sc expansion 闅旂:1M SC 鐨?expansion 涓嶈 4K/64K 骞跺彂 alloc 骞叉壈;`status` 瀹堟亽 + per-thread TLS 闅旂 |
| `case_20_lazy_init_headroom.txt` | lazy 1M + `headroom=512`(`thread_0 alloc 524288 1 512`) | lazy init 涓?headroom 鍙犲姞:alloc 鏃舵惡甯?headroom 鍏冩暟鎹?buf_size 鍙嶆槧 block_size + headroom |

### 4.7 without_data 璺緞鐢ㄤ緥(鏂板 case_17_without_data)

> **缂栧彿璇存槑**:涓?搂4.6 鐨?`case_17_lazy_init_multi_expand` 鍏辩敤缂栧彿 case_17銆俠atch 璺戞壒鏃朵袱鑰呴兘浼氭墽琛屸€斺€斿缓璁湭鏉ラ噸缂栧彿(瑙?搂10 宸茬煡闂)銆?
楠岃瘉 `request_size=0` 鐨?without_data mempool 璺緞:鐙珛 buf pool,鍙€?`umq_buf_t` header(鎻忚堪绗?涓嶅甫 data region銆?
| 鐢ㄤ緥 | 鍦烘櫙 | 楠岃瘉鐐?|
|---|---|---|
| `case_17_without_data.txt` | `scaleCap=off`,threads=2;4 闃舵:info 鈫?鍗曠嚎绋?alloc 0 5 + status 鈫?free all + status 鈫?澶氱嚎绋嬪苟鍙?alloc 0 10 + free all | (1) `DoAlloc` 鎺ュ彈 size=0(姝ゅ墠鎷掔粷涓?invalid) (2) `umq_normal_qbuf_alloc(0,...)` 璺敱鍒?without_data 璺緞(`umq_qbuf_pool.c:1942`) (3) buf 杈撳嚭 `sc=- ... [nodata]`(鏃?SC 姒傚康) (4) DFX 娈? Per-Thread TLS Pool Stats (WithoutData) 璁℃暟澧為暱:`CurBuf`/`AccFetchCnt`/`AccAllocCnt`/`AccFreeCnt` (5) free all 杩?TLS 闈?global(鍚?FAQ Q1) (6) `status` alloced 鍒?*涓?*璁?without_data buf(鍗曠嫭 `without_data: N bufs` 琛? (7) held allocs 鏄剧ず `alloc 0 -> nodata, N/M bufs held` (8) 澶氱嚎绋?per-thread TLS WithoutData 闅旂 |

**鍏抽敭閰嶇疆**:`scaleCap=off`(`disable_scale_cap=true`)璁?`init_split_mode_layout` 棰勫垎閰?without_data pool(`umq_qbuf_pool.c:1283-1299`,`head_without_data_count = total_blk_num * UMQ_EMPTY_HEADER_COEFFICIENT`),浣?alloc 0 N 鐩存帴鍛戒腑 global without_data pool 鏃犻渶 expansion銆?
### 4.8 鏂板 init 鍙傛暟鐢ㄤ緥(case_24~27)

琛ラ綈 `qbuf_pool_cfg_t` 涓鍓嶅伐鍏锋湭鏆撮湶鐨勫瓧娈?`disable_malloc_escape`銆乣expansion_threshold`銆乣tls_expand_mem_budget`銆乣explicit_block_sizes[]`銆乣per_sc_weights[]`銆?
| 鐢ㄤ緥 | 鍦烘櫙 | 楠岃瘉鐐?| 閫€鍑虹爜 |
|---|---|---|---|
| `case_24_escape_disabled.txt` | `escape=off` + 灏?pool(4M),alloc 100(鑼冨洿鍐?+ alloc 700(瓒?pool) | (1) init 娈? `disable_malloc_escape=1` (2) 鑼冨洿鍐?alloc 鎴愬姛 (3) 瓒?pool alloc **澶辫触**(ret鈮?),鏃?escape fallback (4) 娈? `escape_buf_cnt=0`(鏃?escape buf 鍒涘缓)銆傚姣旈粯璁?`escape=on`:鍚屽満鏅細闈欓粯鍒涘缓 escape buf | 0 |
| `case_25_expansion_threshold.txt` | `expThreshold=90`(婵€杩涙棭 expansion) + alloc 100 | (1) init 娈? `expansion_threshold=90` (2) expansion 鍦?90% 姘翠綅瑙﹀彂(姣旈粯璁?30% 鏇存棭)銆傛敞:`expThreshold=0` 涓嶆姤閿?鐢熶骇浠ｇ爜鍙栭粯璁?30(`:1223-1224`);`expThreshold=200` 瓒婄晫 init 杩斿洖 EINVAL(`:1225-1228`) | 0 |
| `case_26_tls_expand_budget.txt` | `tlsBudget=96M` + `tlsExpandBudget=1M` + 2 绾跨▼鍚?alloc 100 | (1) init 娈? `tls_expand_qbuf_pool_depth=1048576` (2) 娈? 姣忕嚎绋?`CurCap 鈮?256`锛?M/4K=256锛屽姣旈粯璁?84M/4K=21504锛?(3) 娈? 姣忕嚎绋?`sc0 tls_cap_cnt 鈮?256`銆傝瘉鏄?per-thread cap 鐙珛浜庡叏灞€ `tlsBudget` | 0 |
| `case_27_blockSizes_weights_field_mapping.txt` | `blockSizes=4K,64K` `weights=2,1` 姝ｅ悜(瀵归綈鐢熶骇榛樿) | (1) init 娈? `explicit_block_sizes=[4096,65536]` (2) 娈? `sc0 initial_blocks` 绾︿负 `sc1` 鐨?32x(`2x weight 脳 64K/4K`)(3) 璺?SC alloc/free 姝ｅ父銆?*瀵归綈鐢熶骇**:`UmqSetting::UMQ_EXPLICIT_BLOCK_SIZES={4096,65536}` `UMQ_PER_SC_WEIGHTS={2,1}` 缁?`umq_backend.cpp:67-72` memcpy 璺緞,宸ュ叿涓庣敓浜ц蛋鐩稿悓浠ｇ爜璺緞 | 0 |

**瀛楁瀵圭収**:

| init 鍙傛暟 | cfg 瀛楁 | 榛樿鍊?=0 鏃? | 鐢熶骇娑堣垂鐐?|
|---|---|---|---|
| `escape` | `disable_malloc_escape`(鍙栧弽) | false(escape 鍚敤) | `umq_qbuf_pool.c:2034/2143/2182/2481` |
| `expThreshold` | `expansion_threshold` | 30(`QBUF_POOL_DEFAULT_EXPANSION_THRESHOLD`) | `umq_qbuf_pool.c:1111/1229/1311/1330/1393/1412` |
| `tlsExpandBudget` | `tls_expand_mem_budget` | 7/8 脳 `tlsBudget`(`umq_qbuf_pool_expand_max`) | `umq_qbuf_pool.c:1214-1216/1666-1669` |
| `blockSizes` | `explicit_block_sizes[]` | 鏈鈫抈mult` 鍑犱綍绾ф暟鍥為€€(`base * mult^i`) | `umq_backend.cpp:67-69`(memcpy `UmqSetting::UMQ_EXPLICIT_BLOCK_SIZES`) |
| `weights` | `per_sc_weights[]` | 鏈鈫掑叏 0鈫掔敓浜у厹搴?`{1,1}` | `umq_backend.cpp:70-72`(memcpy `UmqSetting::UMQ_PER_SC_WEIGHTS`),`umq_qbuf_pool.c:1586-1599`(榛樿鍥為€€) |


## 5. status 杈撳嚭绀轰緥锛坈ase_06 actions 娈靛悗锛?
```
====================[ status ]====================
actions (8):
  [1] init threads=3 count=2 mode=split scaleCap=on allocSz=209715200 poolMaxSz=0 expSlotSz=0 blockSizes=4096,65536 weights=default(1,1) escape=on expThreshold=0 tlsExpandBudget=0
  [2] [t0] alloc 4096 -> sc=0, 1 buf
  [3] [t0] alloc 32768 -> sc=1, 1 buf
  [4] [t1] alloc 65536 -> sc=1, 1 buf
  [5] [t1] alloc 70000 -> sc=1, 2 bufs
  [6] [t1] free 1 -> 1 buf
  [7] [t2] alloc 70000 -> sc=1, 2 bufs
  [8] [t2] free 0 -> 1 buf
held allocs:
  [t0][2] alloc 4096 -> sc=0, 1/1 buf held (free 0)
  [t0][3] alloc 32768 -> sc=1, 1/1 buf held (free 1)
  [t1][4] alloc 65536 -> sc=1, 1/1 buf held (free 2)
  [t1][5] alloc 70000 -> sc=1, 1/2 bufs held (free 3-4)
  [t2][7] alloc 70000 -> sc=1, 1/2 bufs held (free 5-6)
pool state (per-sc global/alloced/exp + per-thread tls):
  sc | blk_size | global | alloced | exp
  0  | 4096     | 2816   | 1        | 0
  1  | 65536    | 2816   | 5        | 0
  per-thread TLS:
    t0 (tid=1265)
      sc0: buf=63   cap=64
      sc1: buf=8    cap=64
    t1 (tid=1266)
      sc0: buf=0    cap=0
      sc1: buf=16   cap=64
    t2 (tid=1267)
      sc0: buf=0    cap=0
      sc1: buf=8    cap=64
  Pool OS Mem Claimed: 209715200 bytes (initial=209715200 + expansion=0 + escape=0)
==================================================
```

### 涓夋鍒嗗伐

1. **actions** 鈥?瀹屾暣鍔ㄤ綔鍘嗗彶锛堝惈 `[tK]` 鍓嶇紑锛宎lloc/free 閮借褰曪級
2. **held allocs** 鈥?褰撳墠鎸佹湁锛堝幓鎺?alloc 鍙?free 鐨勶紝`[tK][actions_idx]` 鏍煎紡锛屾樉绀?`remaining/total` 濡?`1/2 bufs held`锛屾嫭鍙峰唴 `(free N)` 鎴?`(free N-M)` 鎻愮ず璇?op 鐨勭ǔ瀹?orig_idx 鑼冨洿锛?3. **pool state** 鈥?per-sc `global/alloced/exp`锛堝叏灞€鎬诲拰锛? `per-thread TLS` 瀛愯〃锛堟瘡 thread 涓€鍧楀惈 kernel tid锛宻c 鍒嗚 `buf/cap`锛? `Pool OS Mem Claimed` 琛岋紙initial + expansion + escape 涓夐儴鍒嗘€诲拰锛?
### 瀹堟亽涓€鐪煎彲瑙?
姣?sc `global + tls + alloced` 鎭掑畾銆傚 sc0锛歚global 2816 + tls 63 + alloced 1 = 2880`锛坕nit 鎬诲潡鏁帮級锛泂c1锛歚global 2816 + tls(t0 8 + t1 16 + t2 8) 32 + alloced 5 = 2853` 鈮?2880锛坅lloc 70000 鍗犵敤 2 涓?64K buf锛屾晠 tls 鐣ュ浜?fetch锛夈€?
> **娉?*锛歚tid` 鏄?kernel TID锛坄syscall(SYS_gettid)`锛屽皬鏁板瓧锛夛紝涓庣敓浜т唬鐮?`g_thread_cache.stats.tid` 涓€鑷达紱闈?`pthread_self()` 鍦板潃锛堟棫鏂囨。绀轰緥濡?`129814476351168` 鏄?pthread_t 鍦板潃锛屽凡搴熷純锛夈€俙cap` 鏄?TLS per-SC 瀹归噺涓婇檺锛坈ount锛岄潪 bytes锛夛紝`buf` 鏄綋鍓?TLS 缂撳瓨鐨?buf 鏁般€?
## 6. info 杈撳嚭锛堝叏閲忥紝~9 娈碉級

`info` 璋冪敤 `umq_qbuf_pool_stats_to_str` 杈撳嚭 DFX 瀛楃涓诧紝鎸?init 楠岃瘉娴佺▼鎺掑簭锛坰ummary 鈫?config 鈫?per-sc 鈫?娲剧敓鎸囨爣锛夛細

| 娈?| 鏍囬 | 鍏抽敭鍒?| 璇存槑 |
|----|------|--------|------|
| 1 | Global Pool Config | Type/Mode/TotalSize/TotalBlk/BlkSize/Headroom/DataSize/BufSize/UmqBufSize/FreeBlk/FreeSize/NoBufFreeBlk/NoBufFreeSize | 姹犳€昏锛坮aw 鏁板€硷級锛屽惈 without_data pool 鐨?NoBuf 鍒?|
| 2 | Pool Config [Normal] | size_class_count/blk_size[]/per_sc_block_count/disable_scale_cap/disable_malloc_escape/expansion_size/expansion_threshold/batch_count/expansion_mem_size_max/exp_total_mem_pool_size/tls_qbuf_pool_depth/tls_expand_qbuf_pool_depth/exp_slot_used_count | 閰嶇疆鍙傛暟锛坕nit key=value 鐩存帴瀵圭収锛夛紱`blk_size[]` 鏄?`explicit_block_sizes[]` 鐨?DFX 鍒悕锛?*`per_sc_weights[]` 涓嶅湪 DFX 杈撳嚭涓?*锛堢敓浜т唬鐮佸唴閮ㄤ娇鐢紝浠呭湪 init 鏃跺奖鍝?`per_sc_block_count` 鍒嗛厤锛?|
| 3 | Per-SizeClass State | sc/blk_size/glbl_free/hdr_free/exp_slots/exp_total_blk/exp_total_exp/exp_total_shrink/glbl_total/cap/exp_free/trig_expand | per-sc 鐘舵€侊紱`glbl_free` 鏄?global 鍙敤 buf 鏁帮紙with_data锛夛紱`hdr_free` 鏄?without_data pool 鍙敤鏁帮紙sc0 鍒楋級锛沗exp_slots`/`exp_free` 鏄?expansion 妲戒綅涓庡彲鐢ㄥ潡锛沗trig_expand` 鏄Е鍙?expansion 鐨勬按浣嶉槇鍊?|
| 4 | Expansion Pool | Type/ExpandCnt/TotalBlk/FreeBlk/MemSize/AccExpCnt/SyncExpCnt/AsyncExpCnt/AccShrinkCnt + `partial_slot_count: WithData=N WithoutData=N` | WithData + WithoutData 涓よ + partial_slot_count 鍗曡锛堥儴鍒嗗～鍏呮Ы缁熻锛?|
| 5 | Per-Thread TLS Pool Stats (WithData) | Type/TID/CurCap/CurBuf/AccFetchCnt/AccFetchBuf/AccReturnCnt/AccReturnBuf/AccAllocCnt/AccFreeCnt | `total` 琛?+ 姣?thread 涓€琛岋紱`AccAlloc`鈫抈AccAllocCnt`銆乣AccFree`鈫抈AccFreeCnt`锛圕nt 鍚庣紑缁熶竴锛?|
| 6 | Per-Thread TLS Pool Stats (WithoutData) | 鍚屼笂鍒楀悕 | 鍊熺敤 header 鍦烘櫙锛屽鏁版儏鍐典笅鍏?0 |
| 7 | Per-Thread Per-SC TLS (WithData) | sc/tls_buf_cnt/tls_cap_cnt | per-thread per-sc 缁嗗垎锛坄local_qbuf_pool_num==0` 鏃舵暣娈佃烦杩囷級锛沗tls_buf_cnt` 鏄綋鍓?TLS 缂撳瓨 buf 鏁帮紝`tls_cap_cnt` 鏄?per-SC 瀹归噺涓婇檺锛坈ount锛岄潪 bytes锛?|
| 8 | Derived Metrics | Pool/TotalSize/TotalCnt/FreeSize/FreeCnt/Utilization/TlsLocality/AllocMinusFree + 姣忚 `Normal-scN` 瀛愯 | 娲剧敓鎸囨爣锛歚utilization=(total-free)/total%`銆乣tls_locality=tls/(tls+global)%`銆乣alloc_minus_free=AccAlloc-AccFree`锛坙eak 鎺㈤拡锛屽簲=0 鎴?tls cur_buf 鎬诲拰锛夛紱size 鍒楀悓鏃舵樉绀?raw + K/M/G 鍗曚綅锛沺er-SC 瀛愯鐙珛鏄剧ず鍚?SC 鍒╃敤鐜?|
| 9 | Escape | escape_buf_cnt | 鍔?`== [ Escape ] ==` 涓夋寮忔鏋讹紝涓庡叾浠栨涓€鑷?|

## 7. 娴嬭瘯鍦烘櫙瀵圭収琛?
| 娴嬭瘯鐩殑 | 鐢ㄤ緥 / 鍛戒护 | 鍏抽敭瑙傚療鐐?|
|---|---|---|
| 澶氱骇 sc 璺敱姝ｇ‘ | case_01 / `alloc 4096`+`alloc 65536` | alloc 杈撳嚭 `sc=0` / `sc=1`锛沬nfo 娈?3 Per-SizeClass `blk_size=4096/65536` 鍙嶆帹 |
| 澶氱骇缁撴瀯鍒濆鍖?| case_01 `info` 娈?2+3 | 娈?2 `size_class_count=2`銆乣blk_size[0]=4096`/`blk_size[1]=65536`锛涙 3 `blk_size=4096`(sc0) / `65536`(sc1)銆乣glbl_free`/`hdr_free` 鍒嗚 |
| TLS 瀛楄妭棰勭畻鏈哄埗 | case_01 `info` 娈?5+7 alloc 鍓嶅悗 | 娈?5 `CurCap`锛?TLS 鎬?buf 瀹归噺锛夌疮鍔狅紙涓嶉殢 alloc 閫掑噺锛夛紱娈?7 per-sc `tls_cap_cnt` 鏄?per-SC 瀹归噺涓婇檺 |
| TLS fetch 鎵归噺棰勫彇 | case_07 闃舵1 `info` 娈?5 | `AccFetchBuf=320`锛? 鎵姑?4锛夛紝`AccAllocCnt=300` |
| free 杩?TLS 闈?global | case_07 闃舵2 `info` 娈?5 | free all 鍚?`CurBuf` 鍗囧埌 fetch 閲忥紙320锛夛紝`FreeBlk` 涓嶅彉锛坅ctual==cap锛?|
| self_shrink 褰掕繕 global | case_07 闃舵3 `info` 娈?5+娈?8 | alloc 1 瑙﹀彂 shrink锛宍CurBuf` 闄嶃€乣FreeBlk` 鍗囷紱娈?8 `TlsLocality` 鍚屾涓嬮檷 |
| 澶氱嚎绋?TLS 闅旂 | case_05/06 `status` 鎴?`info` 娈?5 | 娈?5 涓嶅悓 `TID`锛坘ernel TID 灏忔暟瀛楋級鐨勮鍚勮嚜 `AccFetchBuf`/`CurBuf` 鐙珛 |
| per-thread 娉勬紡妫€娴?| case_03/06 | `info` 娈?8 `AllocMinusFree > 0`锛堣繍琛屼腑鍙锛夛紱閫€鍑?`NOTE: thread K has N bufs unfreed` |
| 瀹堟亽涓€鐪煎彲瑙?| case_04 `info` 娈?3+5+8 | 姣?sc锛氭 3 `glbl_free` + 娈?5 `CurBuf` + alloced锛?AccAlloc-AccFree锛?鎭掔瓑浜?init 鎬诲潡鏁帮紱娈?8 `Utilization` 涓庝箣瀵归綈 |
| held allocs 杩囨护 | case_04 status | alloc+free 瀵归殣钘忥紝鍙樉绀轰粛鎸佹湁鐨勶紙`remaining/total`锛?|

## 8. 甯歌闂

### Q1: free all 鍚?TLS buf 娌′笅闄嶃€乬lobal 娌″崌锛屼负浠€涔堜笉褰掕繕锛?`umq_qbuf_free` 鎶?buf 杩樺埌 **TLS 缂撳瓨**锛坵ith_data 姹狅級锛屼笉鏄洿鎺?global銆俙return_to_global` 鍙湪 `actual_bytes > cap_bytes`锛坄umq_qbuf_pool.c:1750`锛夎Е鍙戯紝`cap_bytes = bytes_with_data[sc]`锛坒etch 鏃剁疮鍔狅級銆俧ree all 鍚?`actual = fetch脳block = bytes_with_data = cap`锛岀浉绛変笉瓒咃紝涓嶅綊杩樸€傚綊杩?global 鍦?`self_shrink`锛坅lloc 璺緞锛宍remaining/4 >= 64`锛夈€傝 case_07 闃舵3銆?### Q2: `alloc 4096 num=300` 鎶?"size/num invalid"锛?
alloc 鐨?num 鏄?*浣嶇疆鍙傛暟**锛坅rgs[2] 鐩存帴 strtoul锛夛紝涓嶆槸 key=value銆俙num=300` 琚В鏋愭垚 0銆傜敤 `alloc 4096 300`锛堜綅缃弬鏁帮級銆俰nit 鎵嶇敤 key=value銆?
**alloc 涓婇檺**锛堜笁灞傦級锛?1. **鍗曟 alloc**: `num 鈮?1000`锛坄DoAlloc` 妫€鏌ワ紝闃叉鍗曟杩囧ぇ锛?2. **per-worker 绱**: `alloced.size() 鈮?WORKER_ALLOCED_CAP=65536`锛堥槻姝?`alloced` vector 鏃犵晫澧為暱 + O(n) 鎵弿閫€鍖栵級銆傝秴闄愭椂 `DoAlloc` 杩斿洖閿欒锛屾彁绀?"Free some bufs first or use `free all` to reset"
3. **姹犲瓙鑰楀敖**: `umq_normal_qbuf_alloc` 杩斿洖 ENOMEM锛堣嚜鐒朵笂闄愶紝閫氬父鏈€鍏堣Е鍙戯級

per-worker 涓婇檺鏄畨鍏ㄧ綉鈥斺€旀睜瀛愰€氬父鍙湁 ~3072 bufs锛垀106MB allocSz锛夛紝8 worker 骞冲潎姣忎釜 ~384 bufs锛岃繙浣庝簬 65536銆傚彧鏈夊綋鐢ㄦ埛蹇樿 `free all` 鑰屾寔缁?alloc 鏃舵墠浼氭挒涓婇檺銆?
### Q3: 澶氱嚎绋嬫椂 actions 搴忓彿鍜?held allocs 搴忓彿鎬庝箞瀵瑰簲锛?held allocs 鐨?`[tK][N]` 涓?`N` 鏄?`actions_idx+1`锛屽搴?actions 娈电殑搴忓彿锛堝惈 alloc+free 瀵癸紝alloc 鎿嶄綔鐨勫簭鍙凤級銆俛lloc 鍙?free 鐨?`remaining=0` 鏃朵粠 held allocs 闅愯棌锛屼絾 actions 娈典繚鐣欏叏鍘嗗彶銆?### Q4: `thread_K free <orig_idx>` 鐨?orig_idx 鏄叏灞€杩樻槸 per-thread锛?
**per-thread**銆俙thread_K free <orig_idx>` 鍙?free worker K 鑷繁鐨?`alloced` 涓?`orig_idx` 鍖归厤鐨?buf锛屼笉鏀寔璺ㄧ嚎绋?free銆?
**orig_idx 璇箟**锛堢ǔ瀹氱储寮曪紝涓嶄細鍥?erase 鍓嶇Щ锛夛細
- 姣忎釜 buf 鍦?drain 鏃跺垎閰嶄竴涓崟璋冮€掑鐨?`orig_idx`锛堜粠 0 寮€濮嬶級
- `free N` 閫氳繃鎵弿 `alloced` 鏌ユ壘 `orig_idx == N` 鐨?buf锛宔rase 璇?buf 鍚?*鍏朵粬 buf 鐨?orig_idx 涓嶅彉**
- `free all` 娓呯┖ `alloced` 骞堕噸缃?`next_orig_idx = 0`锛屼笅娆?alloc 浠?0 閲嶆柊寮€濮?- `held allocs` 娈垫瘡琛屾湯灏剧殑 `(free N)` 鎴?`(free N-M)` 鎻愮ず璇?op 鐨?orig_idx 鑼冨洿锛岀敤鎴风洿鎺ョ収鎶勫嵆鍙?
**涓庢棫鐗堟湰锛坋rase 鍓嶇Щ锛夌殑鍖哄埆**锛氭棫鐗?`free 0; free 1; free 2; free 3; free 4` 鍥?erase 鍓嶇Щ鍙噴鏀?3 涓?buf锛坆0/b2/b4锛夛紝鍓?2 涓硠婕忥紱鏂扮増绋冲畾绱㈠紩涓?5 涓叏閮ㄦ纭噴鏀俱€?
### Q5: 閫€鍑虹爜鎬庝箞鐪嬶紵
0=鍏ㄩ儴鍛戒护鎴愬姛锛岄潪0=鏈夊懡浠ゅけ璐ワ紙ERROR 鎵撳埌 stderr锛夈€傝剼鏈粨鏉熻嚜鍔?Cleanup锛堟硠婕忔鏌?+ join workers + uninit锛夈€?
### Q6: 涓轰粈涔?`info` 鐨?TLS 娈佃閬嶅巻 `g_tls_register_head`锛?`g_thread_cache` 鏄?`__thread`锛屼富绾跨▼璁块棶鍙湅涓荤嚎绋?TLS锛堢┖锛屼富绾跨▼涓嶅垎鍙戜笟鍔★級銆傝鐪嬫墍鏈?worker TLS锛屽繀椤婚亶鍘嗗叏灞€閾捐〃 `g_tls_register_head`锛堝姞閿?`g_tls_stats_lock`锛夈€?
### Q7: 骞惰鍧楀唴鐨?buf[i] 杈撳嚭鏄寜 worker 椤哄簭鎵撳嵃鐨勶紝鐪嬭捣鏉ュ儚涓茶锛熸槸骞惰鐨勫悧锛?
**鏄苟琛岀殑鈥斺€攚orker 鎵ц骞跺彂锛屼絾 drain锛堟墦鍗帮級鐢变富绾跨▼涓茶鍋?*銆?
璁捐濡傛锛氫富绾跨▼鍦?`SyncAll()` 涓寜 `g_workers` 椤哄簭锛坵orker idx 鍗囧簭锛塦pthread_cond_wait(done)` + drain `task.alloc_list`锛屼繚璇?`g_actions` 搴忓彿绋冲畾锛堟寜娲惧彂椤哄簭锛岄潪瀹屾垚椤哄簭锛夈€傛墍浠?buf[i] 杈撳嚭椤哄簭鏄?t0 鍏ㄩ儴銆乼1 鍏ㄩ儴銆佲€︺€乼7 鍏ㄩ儴鈥斺€旇繖**鍙弽鏄?drain 椤哄簭锛屼笉鍙嶆槧鎵ц椤哄簭**銆?
瑕侀獙璇?worker 鐪熷苟鍙戞墽琛岋紝寮€ verbose 妯″紡锛?
```bash
QBUF_TOOL_VERBOSE=1 ./build_qbuf_tool/qbuf_pool_tool case_08_concurrent_alloc_race.txt 2>&1 1>/dev/null
```

杈撳嚭绀轰緥锛堝叧閿瘉鎹級锛?```
[parallel] dispatched 8 commands in 158渭s, entering barrier (workers now running concurrently)
[sync] t0 drained: worker ran [95925..95933]渭s (dur=8渭s), main waited 0渭s for done
[sync] t1 drained: worker ran [95936..95959]渭s (dur=23渭s), main waited 0渭s for done
[sync] t2 drained: worker ran [95946..95964]渭s (dur=18渭s), main waited 1渭s for done
...
[sync] barrier complete: total=346渭s, parallel_exec_window=152渭s,
       per-worker wait: min=0渭s max=1渭s avg=0渭s
```

**鍒ゆ柇鏍囧噯**锛?1. **per-worker wait 鈮?0**锛坢ax=1渭s锛夛細涓荤嚎绋嬪埌杈?SyncAll 鏃?8 涓?worker 閮藉凡瀹屾垚 鈫?蹇呯劧骞跺彂锛堜覆琛岀殑璇濇瘡涓?wait 鈮?worker 鎵ц鏃堕棿锛?2. **worker 鍖洪棿閲嶅彔**锛歵1=[95936..95959] 涓?t2=[95946..95964] 鍦?[95946..95959] 閲嶅彔 13渭s 鍚屾椂璺?3. **parallel_exec_window < 危(worker dur)**锛?52渭s < 8+23+18+4+3+9+7+5=77渭s 鐨?8 鍊?鈥?骞跺彂鑺傜渷浜?~6 鍊嶆椂闂?
Verbose 妯″紡榛樿鍏抽棴锛宻tress 鐢ㄤ緥(case_10/12)鑻ュ紑 verbose 浼氫骇鐢熷ぇ閲?timing 琛岋紝寤鸿鍙湪璋冭瘯 race 鏃跺惎鐢ㄣ€?

### Q8: `repeat` 鍜?`stress_repeat` 鎬庝箞閫?

| 缁村害 | `repeat N` | `stress_repeat` |
|------|-----------|-------------------|
| 涓婇檺 | N鈮?0000 | **鏃犻檺寰幆** |
| 閫€鍑烘潯浠?| 璺戝畬 N 娆?| Ctrl-C(SIGINT/SIGTERM)鎴?alloc/free 澶辫触 |
| g_actions | 姣忚疆 push,绾挎€у闀?| 涓?push,鎭掑畾 |
| per-buf printf | 姣忚疆鎵撳嵃 | 璺宠繃 |
| Worker::alloc_ops | 涓嶄富鍔ㄦ竻(鐢?`free all` 娓? | 姣忚疆鏈熬闈欓粯娓?閲嶇疆 `next_orig_idx`(褰?`alloced.empty()`) |
| 閫傜敤鍦烘櫙 | 璋冭瘯绔炴€併€佹斁澶?race window | 澶╃骇鍒暱绋炽€佸唴瀛樺帇鍔涢獙璇?|
| 鍑洪敊鏃惰皟璇?| g_actions 瀹屾暣鍘嗗彶鍙洖婧?| 鍙湁 iter 鍙?+ 澶辫触琛?stress 妯″紡涓嶈鍘嗗彶) |
| batch 鑷姩鍖?| 鉁?鍙湪 `for f in cases/*.txt; do ...` 涓窇 | 鉂?鏃犻檺寰幆,涓嶈兘杩?batch(浼氬崱浣? |

**寤鸿**:
- 鐭祴璇?N鈮?0000)銆侀渶瑕?action 鍘嗗彶銆佽嚜鍔ㄥ寲 batch 鈫?`repeat`
- 闀跨ǔ(鎵嬪姩鎺у埗鏃堕暱)銆侀獙璇佸唴瀛樻亽瀹?鈫?`stress_repeat` + `snapshot N`,Ctrl-C 鎺у埗
- 璋冭瘯 race 鏃跺厛鐢?`repeat` 璺戝皬 N 鐪?action 鍘嗗彶,纭閫昏緫鏃犺鍚庡啀鍒?`stress_repeat` 闀胯窇

### Q9: `stress_repeat` 璺戝埌涓€鍗?OOM 浜嗘€庝箞鍔?

`stress_repeat` 璁捐涓婁繚璇佸伐鍏疯嚜韬唴瀛樻亽瀹?g_actions 涓嶅闀裤€乤lloc_ops 姣忚疆娓呯┖銆乧ounters 1 涓?struct)銆傚鏋?RSS 浠嶅闀?鎺掓煡:

1. **鐢ㄦ埛娉勬紡**:`status` 鐨?`stress progress` 娈电湅 `alloced_held`鈥斺€旇嫢闈?0 涓旀寔缁闀?璇存槑姣忚疆 alloc/free 鏈厤瀵广€備慨澶嶈剼鏈€?2. **pool expansion**: `status` 鐨?`Pool OS Mem Claimed` 琛岀湅 `initial + expansion + escape`鈥斺€旇嫢 expansion 鎸佺画澧為暱,璇存槑鐢熶骇浠ｇ爜 expansion 娌″洖鏀?鍙兘 pool 閰嶇疆闂,妫€鏌?`poolMaxSz` cap)銆俻ool 鑷韩鍙?`poolMaxSz` CAS 绾︽潫,鐞嗚涓婇檺纭畾銆?3. **glibc malloc arena**:澶氱嚎绋嬪満鏅?glibc 浼氫负姣忎釜绾跨▼鍒嗛厤 malloc arena(榛樿 8脳CPU 鏍告暟)銆傜敤 `MALLOC_ARENA_MAX=2 ./build_qbuf_tool/qbuf_pool_tool ...` 闄愬埗 arena 鏁?鍙樉钁楅檷 RSS銆?4. **buf printf 鎷栫暀**:stress 妯″紡涓?per-buf printf 宸茶烦杩?鑻ヤ粛鐪嬪埌澶ч噺 stdout 杈撳嚭,妫€鏌ユ槸鍚﹁鐢ㄤ簡 `repeat` 鑰岄潪 `stress_repeat`銆?

## 9. 鏂囦欢娓呭崟

```
test/urpc/umq/qbuf_pool_tool/
鈹溾攢鈹€ CMakeLists.txt          # 鐙珛 cmake 椤圭洰
鈹溾攢鈹€ qbuf_pool_tool.h        # struct + 鍏ㄥ眬 extern + 鍑芥暟澹版槑
鈹溾攢鈹€ stubs.cpp               # extern C stubs(涓?include .c)
鈹溾攢鈹€ utils.cpp               # 绾В鏋愬櫒
鈹溾攢鈹€ core.cpp                # tool state + 鍛戒护鍒嗗彂;涓嶅啀 #include umq_qbuf_pool.c
鈹?                          #  (鐢?CMakeLists.txt 缂栧叆 tool target);鎵€鏈?umq pool
鈹?                          #  鐘舵€佽闂蛋鍏叡 DFX API (umq_stats_qbuf_pool_get +
鈹?                          #  umq_qbuf_pool_stats_to_str) 鍜屽叕鍏?umq_qbuf_pool.h
鈹?                          #  鍑芥暟(init/uninit/normal_qbuf_alloc/qbuf_free/...)
鈹?                          #  + DispatchAsync/SyncAll/DrainWorkerResult/
鈹?                          #    DoParallelBlock/DoRepeatBlock/DoStressRepeatBlock/
鈹?                          #    ExecuteBlockLines
鈹溾攢鈹€ main.cpp                # 鑴氭湰璇?+ 鍛戒护鍒嗗彂 + parallel/repeat/stress_repeat 鍧楄В鏋愮姸鎬佹満
鈹斺攢鈹€ cases/                  # 30 涓敤渚?鍚?4 缁勯噸鍙?瑙?搂10)
    鈹溾攢鈹€ case_01_basic_init_alloc_free.txt
    鈹溾攢鈹€ case_02_status.txt
    鈹溾攢鈹€ case_03_leak.txt
    鈹溾攢鈹€ case_04_interleaved_alloc_free.txt
    鈹溾攢鈹€ case_05_multi_thread.txt
    鈹溾攢鈹€ case_06_complex_multi_thread.txt
    鈹溾攢鈹€ case_07_tls_fetch_return.txt
    鈹溾攢鈹€ case_08_concurrent_alloc_race.txt   # 骞惰
    鈹溾攢鈹€ case_09_concurrent_free_race.txt    # 骞惰
    鈹溾攢鈹€ case_10_mixed_alloc_free_race.txt   # 骞惰+repeat
    鈹溾攢鈹€ case_11_cross_sc_parallel.txt       # 骞惰
    鈹溾攢鈹€ case_12_stress_repeat.txt           # 骞惰+repeat
    鈹溾攢鈹€ case_13_threads_beyond_8.txt         # 骞惰(绐佺牬鏃?8 涓婇檺)
    鈹溾攢鈹€ case_14_poolMaxSz_field_mapping.txt  # 瀛楁鏄犲皠姝ｅ悜 (poolMaxSz) 鈥?搂4.3
    鈹溾攢鈹€ case_14_burst_alloc_free_shrink.txt  # burst + shrink 鈥?搂4.5
    鈹溾攢鈹€ case_15_expSlotSz_field_mapping.txt  # 瀛楁鏄犲皠姝ｅ悜 (expSlotSz) 鈥?搂4.3
    鈹溾攢鈹€ case_15_sustained_burst_stability.txt # sustained burst 绋冲畾鎬?鈥?搂4.5
    鈹溾攢鈹€ case_16_field_mapping_rejected.txt   # 瀛楁鏄犲皠璐熷悜 (鏃у悕+瓒婄晫) 鈥?搂4.3
    鈹溾攢鈹€ case_16_lazy_init_3sc.txt            # lazy 1M 鍩虹 鈥?搂4.6
    鈹溾攢鈹€ case_17_lazy_init_multi_expand.txt   # lazy 澶氳疆 expansion 鈥?搂4.6
    鈹溾攢鈹€ case_17_without_data.txt             # without_data 璺緞 鈥?搂4.7
    鈹溾攢鈹€ case_18_lazy_init_combine.txt         # lazy + COMBINE 鈥?搂4.6
    鈹溾攢鈹€ case_19_lazy_init_concurrent.txt      # lazy + 骞跺彂 鈥?搂4.6
    鈹溾攢鈹€ case_20_lazy_init_headroom.txt        # lazy + headroom 鈥?搂4.6
    鈹溾攢鈹€ case_21_stress_repeat.txt            # 闀跨ǔ stress_repeat 鍩虹(鏃犻檺寰幆)
    鈹溾攢鈹€ case_22_stress_infinite.txt          # 闀跨ǔ鏃犻檺寰幆 + RSS 鐩戞帶绀轰緥
    鈹溾攢鈹€ case_23_stress_free_idx.txt          # 闀跨ǔ free <idx> 妯″紡 (alloc_ops 娓呯┖楠岃瘉)
    鈹溾攢鈹€ case_24_escape_disabled.txt          # escape=off + pool 鑰楀敖楠岃瘉 鈥?搂4.8
    鈹溾攢鈹€ case_25_expansion_threshold.txt       # expThreshold 姘翠綅鎺у埗 鈥?搂4.8
    鈹溾攢鈹€ case_26_tls_expand_budget.txt          # tlsExpandBudget per-thread cap 鈥?搂4.8
    鈹斺攢鈹€ case_27_blockSizes_weights_field_mapping.txt  # blockSizes=/weights= 瀛楁鏄犲皠 鈥?搂4.8
```

## 10. 宸茬煡闂

### 10.1 鐢ㄤ緥缂栧彿鍐茬獊(4 缁勯噸鍙?

cases/ 鐩綍瀛樺湪 4 缁勫叡鐢ㄧ紪鍙风殑鏂囦欢:

| 缂栧彿 | 鏂囦欢 A | 鏂囦欢 B |
|---|---|---|
| case_14 | `case_14_poolMaxSz_field_mapping.txt`(搂4.3,閫€鍑虹爜 0) | `case_14_burst_alloc_free_shrink.txt`(搂4.5,閫€鍑虹爜 0) |
| case_15 | `case_15_expSlotSz_field_mapping.txt`(搂4.3,閫€鍑虹爜 0) | `case_15_sustained_burst_stability.txt`(搂4.5,閫€鍑虹爜 0) |
| case_16 | `case_16_field_mapping_rejected.txt`(搂4.3,**閫€鍑虹爜 1**) | `case_16_lazy_init_3sc.txt`(搂4.6,閫€鍑虹爜 0) |
| case_17 | `case_17_lazy_init_multi_expand.txt`(搂4.6,閫€鍑虹爜 0) | `case_17_without_data.txt`(搂4.7,閫€鍑虹爜 0) |

**褰卞搷**:
1. `for f in cases/*.txt; do ./build_qbuf_tool/qbuf_pool_tool "$f"; done` 璺戞壒鏃跺悓鍙风敤渚嬮兘浼氭墽琛屸€斺€攃ase_16 涓€涓€€鍑?0 涓€涓€€鍑?1,鑷姩鍖栫粨鏋滈毦浠ユ寜缂栧彿褰掑洜
2. `cases/` 鐩綍鎸夊瓧姣嶅簭鎺掑垪鏃?`case_16_field_mapping_rejected.txt` 鎺掑湪 `case_16_lazy_init_3sc.txt` 涔嬪墠;涓よ€呴兘璺戞椂鏃ュ織娣锋潅

**寤鸿淇**:lazy init 绯诲垪(case_16~20 鍏?5 涓?+ burst 绯诲垪(case_14_burst銆乧ase_15_sustained 鍏?2 涓?+ without_data(case_17_without_data 1 涓?閲嶇紪鍙蜂负 case_27~34,閬垮厤涓庡瓧娈垫槧灏勭敤渚嬪啿绐?case_24~26 宸茶鏂板鐨?init 鍙傛暟鐢ㄤ緥鍗犵敤,瑙?搂4.8)銆傞噸缂栧彿鍚庡悓姝ユ洿鏂版湰鏂囦欢 搂4.5~搂4.7 涓?搂9 鏂囦欢娓呭崟銆?
### 10.2 娴嬭瘯澹扮О涓嶆敮鎸佺殑瀛楁 鈥?瀹為檯鎯呭喌

娴嬭瘯鏇惧０绉板伐鍏蜂笉鏀寔 5 涓?init 閫夐」,瀹為檯鏍告煡(`qbuf_pool_cfg_t` 瀹氫箟瑙?`umq_qbuf_pool_base.h:85-113`):

| 瀛楁 | 娴嬭瘯澹扮О | 瀹為檯 | 璇存槑 |
|---|---|---|---|
| `umq_buf_pool_max_size` | 涓嶆敮鎸?| **宸叉敮鎸?* | init 鍙傛暟 `poolMaxSz`,瑙?搂3.1銆俢ase_14_poolMaxSz/case_16_field_mapping 宸查獙璇?|
| `disable_malloc_escape` | 涓嶆敮鎸?| 宸茶ˉ榻?搂4.8) | init 鍙傛暟 `escape`,case_24 楠岃瘉 |
| `expansion_threshold` | 涓嶆敮鎸?| 宸茶ˉ榻?搂4.8) | init 鍙傛暟 `expThreshold`,case_25 楠岃瘉 |
| `tls_expand_mem_budget` | 涓嶆敮鎸?| 宸茶ˉ榻?搂4.8) | init 鍙傛暟 `tlsExpandBudget`,case_26 楠岃瘉 |
| `explicit_block_sizes` | (commit 600d24a 鏂板瀛楁) | 宸茶ˉ榻?搂4.8) | init 鍙傛暟 `blockSizes`,case_27 楠岃瘉銆傛鍓嶅伐鍏风敤 `mult` 鍑犱綍绾ф暟杩戜技,鍑戝阀涓庣敓浜?`{4K, 64K}` 涓€鑷翠絾鏃犳硶琛ㄨ揪闈炲嚑浣曞垎甯?|
| `per_sc_weights` | (commit 600d24a 鏂板瀛楁) | 宸茶ˉ榻?搂4.8) | init 鍙傛暟 `weights`,case_27 楠岃瘉銆傛鍓嶅伐鍏?`cfg.per_sc_weights[]` 鍏?0鈫掔敓浜т唬鐮佸厹搴?`{1,1}`,涓庣敓浜?`{2,1}` 涓嶄竴鑷?|
| `lazy_init_block_size_threshold` | (鏃у瓧娈?宸茬Щ闄? | **宸插垹闄?* | commit `600d24a` 绉婚櫎璇ュ瓧娈?鏀逛负 per-SC `per_sc_weights[i]==0` 琛ㄨ揪 lazy銆傚伐鍏?`lazyThreshold` 鍙傛暟鍚屾绉婚櫎,鏃х敤渚嬭縼绉诲埌 `weights=1,1,0`(瑙?搂4.6) |
| `size_class_step_multiplier` | (鏃у瓧娈?宸茬Щ闄? | **宸插垹闄?* | commit `600d24a` 绉婚櫎璇ュ瓧娈?鏀逛负鏄惧紡鏁扮粍 `explicit_block_sizes[]`銆傚伐鍏峰師 `mult` 鍙傛暟(鍑犱綍绾ф暟鍥為€€)鍜?`base` 鍙傛暟(鏄惧紡鏋氫妇)宸插交搴曠Щ闄も€斺€擿base` 鐜颁粠 `blockSizes[0]` 鑷姩鎺ㄥ,`blockSizes=` 蹇呭～ |
| `batch_mem_size` | 涓嶆敮鎸?| **瀛楁涓嶅瓨鍦?* | `qbuf_pool_cfg_t` 涓棤姝ゅ瓧娈?鍏ㄤ粨 grep 杩斿洖 0 鍖归厤銆傜枒涓烘嫾鍐欓敊璇垨骞昏瀛楁鍚?闇€鍚戞祴璇曞洟闃熸牳瀹炴潵婧?|

### 10.3 build_qbuf_tool/ 鏋勫缓浜х墿鐩綍

`qbuf_pool_tool/build_qbuf_tool/` 鍖呭惈 `CMakeCache.txt`/`Makefile`/`qbuf_pool_tool` 浜岃繘鍒剁瓑鏋勫缓浜х墿銆傛瀯寤轰骇鐗╀笉搴旇繘 git 璺熻釜鈥斺€旈渶纭 `.gitignore` 鏄惁宸插拷鐣?鑻ュ凡 commit,搴?`git rm -r --cached qbuf_pool_tool/build_qbuf_tool` 娓呯悊骞跺姞鍏?`.gitignore`銆?