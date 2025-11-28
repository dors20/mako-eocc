# Testing OCC Analysis Tools

## Quick Test (Temporary Integration)

### Step 1: Backup and Replace Scheduler

```bash
cd /home/ubuntu/mako/src/deptran/occ
cp scheduler.cc scheduler_original.cc.bak
cp scheduler_instrumented.cc scheduler.cc
```

### Step 2: Rebuild

```bash
cd /home/ubuntu/mako/build
cmake ..
make -j$(nproc) dbtest
```

### Step 3: Run Test

```bash
cd /home/ubuntu/mako
./build/dbtest --bench tpcc --config config/occ.yml \
    --num-threads 24 \
    --runtime 10 \
    --warehouses 10
```

### Step 4: Check Output

The analysis statistics should be printed at the end of the run. Look for:
- "========== OCC Abort Statistics =========="
- "========== OCC Performance Statistics =========="
- "========== False Abort Analysis =========="

### Step 5: Restore Original

```bash
cd /home/ubuntu/mako/src/deptran/occ
mv scheduler_original.cc.bak scheduler.cc
```

---

## Proper Integration (Permanent)

See `ANALYSIS_INTEGRATION_GUIDE.md` for step-by-step instructions to integrate analysis tools into `scheduler.cc` with `#ifdef OCC_ANALYSIS_ENABLED` guards.

---

## Run Full Analysis Suite

Once integrated, run the full analysis script:

```bash
./scripts/run_occ_analysis.sh config/occ.yml
```

This will run tests with varying contention levels and save results to `results/occ_analysis_*/`

---

## Analyze Results

```bash
python3 scripts/analyze_occ_results.py results/occ_analysis_*/
```

