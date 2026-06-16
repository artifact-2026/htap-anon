//
//  ycsbc.cc
//  YCSB-C
//
//

#include <cstring>
#include <string>
#include <iostream>
#include <fstream>
#include <vector>
#include <future>
#include <numeric>
#include <cmath>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include "core/utils.h"
#include "core/timer.h"
#include "core/client.h"
#include "core/core_workload.h"
#include "db/db_factory.h"

using namespace std;

////statistics
atomic<uint64_t> ops_cnt[ycsbc::Operation::READMODIFYWRITE + 1];
atomic<uint64_t> ops_time[ycsbc::Operation::READMODIFYWRITE + 1]; 
////

struct run_result
{
  int oks;
  std::vector<uint64_t> xput;
  std::vector<double> exec_time;

  run_result(int o, int xput_size, int exec_time_size) 
  : oks(o), xput(xput_size, 0), exec_time(exec_time_size, 0) {}
};

void UsageMessage(const char *command);
bool StrStartWith(const char *str, const char *pre);
string ParseCommandLine(int argc, const char *argv[], utils::Properties &props);
void Init(utils::Properties &props);
void PrintInfo(utils::Properties &props);
void write_csv_row(const std::vector<double>& rr, const std::string& filename);
void runLoad(utils::Properties &props, int num_threads, ycsbc::DB *db, bool print_stats, std::vector<std::unique_ptr<ycsbc::CoreWorkload>>& wls);
void runXput(utils::Properties &props, int num_threads, ycsbc::DB *db, int run_time, bool print_stats, std::vector<std::unique_ptr<ycsbc::CoreWorkload>>& wls);

struct run_result DelegateClient(ycsbc::DB *db, ycsbc::CoreWorkload *wl, const int num_ops,
    bool is_loading) {
  std::cerr << "[Debug] DelegateClient started with db=" << db << " wl=" << wl << " num_ops=" << num_ops << std::endl;
  ycsbc::Client client(*db, *wl);
  struct run_result rr(0, 0, num_ops);
  int next_report_ = 0;
  for (int i = 0; i < num_ops; ++i) {

    if (i >= next_report_) {
        if      (next_report_ < 1000)   next_report_ += 100;
        else if (next_report_ < 5000)   next_report_ += 500;
        else if (next_report_ < 10000)  next_report_ += 1000;
        else if (next_report_ < 50000)  next_report_ += 5000;
        else if (next_report_ < 100000) next_report_ += 10000;
        else if (next_report_ < 500000) next_report_ += 50000;
        else                            next_report_ += 100000;
        fprintf(stderr, "... finished %d ops%30s\r", i, "");
        fflush(stderr);
    }
    uint64_t exec_start = get_now_micros();
    if (is_loading) {
      rr.oks += client.DoInsert();
    } else {
      rr.oks += client.DoTransaction();
    }
    rr.exec_time[i] = get_now_micros()-exec_start;
  }
  db->Close();
  return rr;
}

// DelegateForThroughput: time-bounded throughput measurement that follows the
// workload spec's operation mix (read/update/insert/scan/rmw proportions) rather
// than issuing only reads or only inserts.  The throughputtype parameter has been
// removed; call client.DoTransaction() to honour NextOperation().
struct run_result DelegateForThroughput(ycsbc::DB *db, ycsbc::CoreWorkload *wl, int runTime) {
  ycsbc::Client client(*db, *wl);
  struct run_result td_oks(0, runTime, runTime);

  int oks = 0;
  uint64_t exectime = 0;
  int i = 0;
  auto start    = std::chrono::steady_clock::now();
  auto step     = start;
  // Hard wall-clock deadline: exit the loop at runTime seconds regardless of
  // how long individual DoTransaction() calls block (e.g. RocksDB write stalls
  // caused by compaction back-pressure).  The old tick-based loop incremented
  // `i` only on 1-second wall-clock gates, so stalls silently extended the run
  // well beyond the intended window.
  auto deadline = start + std::chrono::seconds(runTime);

  while (true) {
    auto now = std::chrono::steady_clock::now();

    // Primary exit: wall-clock deadline reached.
    if (now >= deadline) break;

    // Secondary exit: all per-second buckets filled (defensive, normally the
    // deadline fires first once clock-skew between `step` snaps is corrected).
    if (i >= runTime) break;

    // Flush 1-second buckets for each complete second elapsed.
    // This loop handles cases where the thread was preempted for multiple
    // seconds, ensuring no buckets are lost during high CPU contention.
    while (now - step >= std::chrono::seconds(1)) {
      td_oks.xput[i]     = oks;
      td_oks.exec_time[i] = (oks > 0) ? double(exectime) / double(oks) : 0.0;
      i       += 1;
      oks      = 0;
      exectime = 0;
      step   += std::chrono::seconds(1);
    }

    uint64_t exec_start = get_now_micros();
    // Follow the workload spec's operation mix (read/update/insert/scan/rmw).
    oks += client.DoTransaction();
    exectime += (get_now_micros() - exec_start);
  }

  // Flush the last (possibly partial) bucket so no ops are lost.
  if (i < runTime && oks > 0) {
    td_oks.xput[i]     = oks;
    td_oks.exec_time[i] = double(exectime) / double(oks);
  }

  return td_oks;
}

int main( const int argc, const char *argv[]) {
  utils::Properties props;
  Init(props);
  string file_name = ParseCommandLine(argc, argv, props);

  ycsbc::DB *db = ycsbc::DBFactory::CreateDB(props);
  if (!db) {
    cout << "Unknown database name " << props["dbname"] << endl;
    exit(0);
  }
  db->Init();
  std::cout << "db name: " << props["dbname"] << std::endl;

  const bool load = utils::StrToBool(props.GetProperty("load","false"));
  const bool run = utils::StrToBool(props.GetProperty("run","false"));
  const bool throughput = utils::StrToBool(props.GetProperty("throughput","false"));
  const int num_threads = stoi(props.GetProperty("threadcount", "1"));
  const bool print_stats = utils::StrToBool(props["dbstatistics"]);
  const bool wait_for_balance = utils::StrToBool(props["dbwaitforbalance"]);
  const int run_time = stoi(props.GetProperty("runtime", "600"));
  const int total_ops = stoi(props[ycsbc::CoreWorkload::OPERATION_COUNT_PROPERTY]);

  string morerun = props["morerun"];

  vector<future<struct run_result>> actual_ops;
  int sum = 0;

  // init workload
  std::cerr << "[Debug] Starting workload initialization..." << std::endl;
  const int total_recs = std::stoi(props[ycsbc::CoreWorkload::RECORD_COUNT_PROPERTY]);
  const int base = total_recs / num_threads;
  const int rem  = total_recs % num_threads;
  std::vector<std::unique_ptr<ycsbc::CoreWorkload>> wls(num_threads);
  for (int t = 0; t < num_threads; ++t) {
    const int my_ops   = base + (t < rem ? 1 : 0);
    const long my_start = long(base) * t + std::min(t, rem);

    utils::Properties p_t = props;
    p_t.SetProperty(ycsbc::CoreWorkload::RECORD_COUNT_PROPERTY, std::to_string(my_ops));
    p_t.SetProperty("insertstart", std::to_string(my_start));
    p_t.SetProperty("totalrecordcount", std::to_string(total_recs));

    auto wl = std::make_unique<ycsbc::CoreWorkload>();
    std::cerr << "[Debug] Initializing workload thread " << t << "..." << std::endl;
    wl->Init(p_t);
    wls[t] = std::move(wl);
  }
  std::cerr << "[Debug] Workload initialization finished." << std::endl;

  if( load ) {
    runLoad(props, num_threads, db, print_stats, wls);
  }

  if( throughput ) {
    runXput(props, num_threads, db, run_time, print_stats, wls);
  }

  if( run ) {
    // Peforms transactions
    for(int j = 0; j < ycsbc::Operation::READMODIFYWRITE + 1; j++){
      ops_cnt[j].store(0);
      ops_time[j].store(0);
    }

    actual_ops.clear();
    uint64_t run_start = get_now_micros();
    for (int i = 0; i < num_threads; ++i) {
      actual_ops.emplace_back(std::async(std::launch::async,
        [&, i, total_ops] {                    // <-- capture wls by reference (&), not by value
          return DelegateClient(db, wls[i].get(), total_ops/num_threads, /*is_loading=*/false);
        }));
    }
    assert((int)actual_ops.size() == num_threads);
    sum = 0;
    std::vector<uint64_t> txn_latencies;
    for (auto &n : actual_ops) {
      assert(n.valid());
      struct run_result rres = n.get();
      sum += rres.oks;
      txn_latencies.insert(txn_latencies.end(), rres.exec_time.begin(), rres.exec_time.end());
    }
    uint64_t run_end = get_now_micros();
    uint64_t use_time = run_end - run_start;

    uint64_t temp_cnt[ycsbc::Operation::READMODIFYWRITE + 1];
    uint64_t temp_time[ycsbc::Operation::READMODIFYWRITE + 1];

    for(int j = 0; j < ycsbc::Operation::READMODIFYWRITE + 1; j++){
      temp_cnt[j] = ops_cnt[j].load(std::memory_order_relaxed);
      temp_time[j] = ops_time[j].load(std::memory_order_relaxed);
    }

    if  (txn_latencies.empty()) {
      printf("Error! No run results!\n");
      return -1;
    }

    double latency_mean = std::accumulate(txn_latencies.begin(), txn_latencies.end(), 0.0) / txn_latencies.size();
    double latency_sq_sum = std::accumulate(txn_latencies.begin(), txn_latencies.end(), 0.0, 
        [latency_mean](double acc, uint64_t value) {
            return acc + (value - latency_mean) * (value - latency_mean);
        });
    double latency_stddev = std::sqrt(latency_sq_sum / txn_latencies.size());

    std::sort(txn_latencies.begin(), txn_latencies.end());
    size_t n = txn_latencies.size();
    auto idx = [&](double p){ return txn_latencies[std::min((size_t)std::floor(p*(n-1)), n-1)]; };
    auto p50 = idx(0.50), p99 = idx(0.99), p25 = idx(0.25), p75 = idx(0.75), min = idx(0.0);

    printf("********** run result **********\n");
    printf("all operation records:%d  use time:%.3f s  IOPS:%.2f iops (%.2f us/op)\n\n", 
      sum, 1.0 * use_time*1e-6, 
      1.0 * sum * 1e6 / use_time, 
      1.0 * use_time / sum * num_threads);
    printf("insert ops:%7lu  use time:%7.3f s  IOPS:%7.2f iops (%.2f us/op)\n", 
      temp_cnt[ycsbc::INSERT], 1.0 * temp_time[ycsbc::INSERT]*1e-6, 
      1.0 * temp_cnt[ycsbc::INSERT] * 1e6 / use_time, 
      1.0 * temp_time[ycsbc::INSERT] / temp_cnt[ycsbc::INSERT]);
    printf("read ops  :%7lu  use time:%7.3f s  IOPS:%7.2f iops (%.2f us/op)\n", 
      temp_cnt[ycsbc::READ], 1.0 * temp_time[ycsbc::READ]*1e-6, 
      1.0 * temp_cnt[ycsbc::READ] * 1e6 / use_time, 
      1.0 * temp_time[ycsbc::READ] / temp_cnt[ycsbc::READ]);
    printf("update ops:%7lu  use time:%7.3f s  IOPS:%7.2f iops (%.2f us/op)\n", 
      temp_cnt[ycsbc::UPDATE], 1.0 * temp_time[ycsbc::UPDATE]*1e-6, 
      1.0 * temp_cnt[ycsbc::UPDATE] * 1e6 / use_time, 
      1.0 * temp_time[ycsbc::UPDATE] / temp_cnt[ycsbc::UPDATE]);
    printf("scan ops  :%7lu  use time:%7.3f s  IOPS:%7.2f iops (%.2f us/op)\n", 
      temp_cnt[ycsbc::SCAN], 1.0 * temp_time[ycsbc::SCAN]*1e-6, 
      1.0 * temp_cnt[ycsbc::SCAN] * 1e6 / use_time, 
      1.0 * temp_time[ycsbc::SCAN] / temp_cnt[ycsbc::SCAN]);
    printf("rmw ops   :%7lu  use time:%7.3f s  IOPS:%7.2f iops (%.2f us/op)\n", 
      temp_cnt[ycsbc::READMODIFYWRITE], 1.0 * temp_time[ycsbc::READMODIFYWRITE]*1e-6, 
      1.0 * temp_cnt[ycsbc::READMODIFYWRITE] * 1e6 / temp_time[ycsbc::READMODIFYWRITE] * num_threads, 
      1.0 * temp_time[ycsbc::READMODIFYWRITE] / temp_cnt[ycsbc::READMODIFYWRITE]);
    printf("total requests: %ld, latency mean: %lf, latency stddev: %lf\n", txn_latencies.size(), latency_mean, latency_stddev);
    printf("Min: %ld, P25: %ld, P50: %ld, P75: %ld, P99: %ld\n", min, p25, p50, p75, p99);
    printf("********************************\n");

    if ( print_stats ) {
      printf("-------------- db statistics --------------\n");
      db->PrintStats();
      printf("-------------------------------------------\n");
    }
    
  }
  if( !morerun.empty() ) {
    vector<string> runfilenames;
    size_t start=0,index=morerun.find_first_of(':', 0);
    while(index!=morerun.npos)
    {
        if(start!=index)
            runfilenames.push_back(morerun.substr(start,index-start));
        start=index+1;
        index=morerun.find_first_of(':',start);
    }
    if(!morerun.substr(start).empty()) {
      runfilenames.push_back(morerun.substr(start));
    }
    for(unsigned int i = 0; i < runfilenames.size(); i++){
      for(int j = 0; j < ycsbc::Operation::READMODIFYWRITE + 1; j++){
        ops_cnt[j].store(0);
        ops_time[j].store(0);
      }

      ifstream input(runfilenames[i]);
      try {
        props.Load(input);
      } catch (const string &message) {
        cout << message << endl;
        exit(0);
      }
      input.close();
      printf("------ run:%s ------\n",runfilenames[i].c_str());
      PrintInfo(props);
      // Peforms transactions
      ycsbc::CoreWorkload wl;
      wl.Init(props);

      actual_ops.clear();
      uint64_t run_start = get_now_micros();
      for (int i = 0; i < num_threads; ++i) {
        actual_ops.emplace_back(std::async(std::launch::async,
          [&, i, total_ops] {                    // <-- capture wls by reference (&), not by value
            return DelegateClient(db, wls[i].get(), total_ops/num_threads, /*is_loading=*/true);
          }));
      }
      assert((int)actual_ops.size() == num_threads);
      sum = 0;
      for (auto &n : actual_ops) {
        assert(n.valid());
        struct run_result runres = n.get();
        sum += runres.oks;
      }
      uint64_t run_end = get_now_micros();
      uint64_t use_time = run_end - run_start;

      uint64_t temp_cnt[ycsbc::Operation::READMODIFYWRITE + 1];
      uint64_t temp_time[ycsbc::Operation::READMODIFYWRITE + 1];

      for(int j = 0; j < ycsbc::Operation::READMODIFYWRITE + 1; j++){
        temp_cnt[j] = ops_cnt[j].load(std::memory_order_relaxed);
        temp_time[j] = ops_time[j].load(std::memory_order_relaxed);
      }

      printf("********** more run result **********\n");
      printf("all operation records:%d  use time:%.3f s  IOPS:%.2f iops (%.2f us/op)\n\n", sum, 1.0 * use_time*1e-6, 1.0 * sum * 1e6 / use_time, 1.0 * use_time / sum);
      if ( temp_cnt[ycsbc::INSERT] )          printf("insert ops:%7lu  use time:%7.3f s  IOPS:%7.2f iops (%.2f us/op)\n", temp_cnt[ycsbc::INSERT], 1.0 * temp_time[ycsbc::INSERT]*1e-6, 1.0 * temp_cnt[ycsbc::INSERT] * 1e6 / temp_time[ycsbc::INSERT], 1.0 * temp_time[ycsbc::INSERT] / temp_cnt[ycsbc::INSERT]);
      if ( temp_cnt[ycsbc::READ] )            printf("read ops  :%7lu  use time:%7.3f s  IOPS:%7.2f iops (%.2f us/op)\n", temp_cnt[ycsbc::READ], 1.0 * temp_time[ycsbc::READ]*1e-6, 1.0 * temp_cnt[ycsbc::READ] * 1e6 / temp_time[ycsbc::READ], 1.0 * temp_time[ycsbc::READ] / temp_cnt[ycsbc::READ]);
      if ( temp_cnt[ycsbc::UPDATE] )          printf("update ops:%7lu  use time:%7.3f s  IOPS:%7.2f iops (%.2f us/op)\n", temp_cnt[ycsbc::UPDATE], 1.0 * temp_time[ycsbc::UPDATE]*1e-6, 1.0 * temp_cnt[ycsbc::UPDATE] * 1e6 / temp_time[ycsbc::UPDATE], 1.0 * temp_time[ycsbc::UPDATE] / temp_cnt[ycsbc::UPDATE]);
      if ( temp_cnt[ycsbc::SCAN] )            printf("scan ops  :%7lu  use time:%7.3f s  IOPS:%7.2f iops (%.2f us/op)\n", temp_cnt[ycsbc::SCAN], 1.0 * temp_time[ycsbc::SCAN]*1e-6, 1.0 * temp_cnt[ycsbc::SCAN] * 1e6 / temp_time[ycsbc::SCAN], 1.0 * temp_time[ycsbc::SCAN] / temp_cnt[ycsbc::SCAN]);
      if ( temp_cnt[ycsbc::READMODIFYWRITE] ) printf("rmw ops   :%7lu  use time:%7.3f s  IOPS:%7.2f iops (%.2f us/op)\n", temp_cnt[ycsbc::READMODIFYWRITE], 1.0 * temp_time[ycsbc::READMODIFYWRITE]*1e-6, 1.0 * temp_cnt[ycsbc::READMODIFYWRITE] * 1e6 / temp_time[ycsbc::READMODIFYWRITE], 1.0 * temp_time[ycsbc::READMODIFYWRITE] / temp_cnt[ycsbc::READMODIFYWRITE]);
      printf("********************************\n");

      if ( print_stats ) {
        printf("-------------- db statistics --------------\n");
        db->PrintStats();
        printf("-------------------------------------------\n");
      }
    }
    
  }
  // if ( print_stats ) {
  //   printf("-------------- db statistics --------------\n");
  //   db->PrintStats();
  //   printf("-------------------------------------------\n");
  // }
  if ( wait_for_balance ) {
    uint64_t sleep_time = 0;
    while(!db->HaveBalancedDistribution()){
      sleep(10);
      sleep_time += 10;
    }
    printf("Wait balance:%lu s\n",sleep_time);

    printf("-------------- db statistics --------------\n");
    db->PrintStats();
    printf("-------------------------------------------\n");
  }
  db->Close();
  delete db;
  return 0;
}

void runLoad(utils::Properties &props, int num_threads, ycsbc::DB *db, bool print_stats, std::vector<std::unique_ptr<ycsbc::CoreWorkload>>& wls) {
  std::cerr << "[Debug] Entering runLoad..." << std::endl;
  int total_rds = stoi(props[ycsbc::CoreWorkload::RECORD_COUNT_PROPERTY]);
  vector<future<struct run_result>> actual_ops;
  uint64_t load_start = get_now_micros();
  for (int i = 0; i < num_threads; ++i) {
    std::cerr << "[Debug] Starting thread " << i << " with db=" << db << " wl=" << wls[i].get() << std::endl;
    actual_ops.emplace_back(std::async(std::launch::async,
      [&, i, total_rds] {                    // <-- capture wls by reference (&), not by value
        return DelegateClient(db, wls[i].get(), total_rds/num_threads, /*is_loading=*/true);
      }));
  }
  std::cerr << "[Debug] All threads started. Waiting for completion..." << std::endl;
  assert((int)actual_ops.size() == num_threads);

  int sum = 0;
  int mth = 0; 
  for (auto &n : actual_ops) {
    assert(n.valid());
    struct run_result loadres = n.get();
    if (mth == 1) {
      write_csv_row(loadres.exec_time, "exec_time.csv");
    }
    sum += loadres.oks;
    mth += 1;
  }
  uint64_t load_end = get_now_micros();
  uint64_t use_time = load_end - load_start;
  printf("********** load result **********\n");
  printf("loading records:%d  use time:%.3f s  IOPS:%.2f iops (%.2f us/op)\n", sum, 1.0 * use_time*1e-6, 1.0 * sum * 1e6 / use_time, 1.0 * use_time / sum);
  printf("*********************************\n");

  if ( print_stats ) {
    printf("-------------- db statistics --------------\n");
    db->PrintStats();
    printf("-------------------------------------------\n");
  }
}

void runXput(utils::Properties &props, int num_threads, ycsbc::DB *db, int run_time, bool print_stats, std::vector<std::unique_ptr<ycsbc::CoreWorkload>>& wls) {
    vector<future<struct run_result>> throughput_ops;

    for (int i = 0; i < num_threads; ++i) {
      throughput_ops.emplace_back(std::async(std::launch::async,
        [&, i] {
          return DelegateForThroughput(db, wls[i].get(), run_time);
        }));
    }
    assert((int)throughput_ops.size() == num_threads);

    // run_time is given in number of seconds
    int run_time_in_units = run_time;
    std::vector<uint64_t> xputs(run_time_in_units);
    std::vector<double> exec_times(run_time_in_units);
    //uint64_t total = 0;
    for (auto &n : throughput_ops) {
      assert(n.valid());
      struct run_result th_work = n.get();
      for (int k=0; k < run_time_in_units; k++) {
        xputs[k] += th_work.xput[k];
        exec_times[k] += th_work.exec_time[k];
      }
    }

    for (int k=0; k < run_time_in_units; k++) {
      exec_times[k] /= throughput_ops.size();
    }

    int skip = stoi(props.GetProperty("skip", "60"));
    if ((int)exec_times.size() <= skip || (int)xputs.size() <= skip) {
      skip = 0;
    }
    double mean = std::accumulate(exec_times.begin()+skip, exec_times.end(), 0.0) / (exec_times.size()-skip);
    double sum_of_squares = std::accumulate(exec_times.begin()+skip, exec_times.end(), 0.0, 
        [mean](double acc, int value) {
            return acc + std::pow(value - mean, 2);
        });
    double stddev = std::sqrt(sum_of_squares / (exec_times.size()-skip));

    double mean_xput = std::accumulate(xputs.begin()+skip, xputs.end(), 0.0) / (xputs.size()-skip);
    double sum_of_squares_xput = std::accumulate(xputs.begin()+skip, xputs.end(), 0.0, 
        [mean_xput](double acc, int value) {
            return acc + std::pow(value - mean_xput, 2);
        });
    double stddev_xput = std::sqrt(sum_of_squares_xput / (xputs.size()-skip));

    printf("********** throughput result **********\n");

    //printf("latency raw data: \n");
    //printf("latency size: %ld \n", exec_times.size());

    /*int timesec = 1;
    for (auto th : xputs) {
      //file << timesec << "," << th << std::endl;
      printf("Time (sec): %d, Xput: %ld", timesec, th);
      timesec++;
    }*/
    printf("throughput mean:%lf  stddev: %lf, average latency: %lf, stddev: %lf\n",
        mean_xput, stddev_xput, mean, stddev);
    printf("*********************************\n");

    // Write per-window throughput stats to CSV
    int xput_window = stoi(props.GetProperty("xputwindow", "10"));
    std::string xput_file = props.GetProperty("xputfile", "xput_stats.csv");

    if (xput_window > 0) {
      std::ofstream out(xput_file);
      if (!out) {
        fprintf(stderr, "Warning: could not open xput output file '%s'\n", xput_file.c_str());
      } else {
        out << "window_start_sec,avg_throughput,stddev_throughput,min_throughput,max_throughput\n";
        for (int w = skip; w + xput_window <= run_time_in_units; w += xput_window) {
          double wmean = 0.0;
          for (int k = w; k < w + xput_window; ++k) wmean += xputs[k];
          wmean /= xput_window;
          double wsq = 0.0;
          for (int k = w; k < w + xput_window; ++k) wsq += std::pow(double(xputs[k]) - wmean, 2);
          double wstddev = std::sqrt(wsq / xput_window);
          uint64_t wmin = *std::min_element(xputs.begin() + w, xputs.begin() + w + xput_window);
          uint64_t wmax = *std::max_element(xputs.begin() + w, xputs.begin() + w + xput_window);
          out << w << "," << wmean << "," << wstddev << "," << wmin << "," << wmax << "\n";
        }
        printf("Per-window throughput stats written to '%s' (window=%ds, skip=%ds)\n",
               xput_file.c_str(), xput_window, skip);
      }
    }

    if ( print_stats ) {
      printf("-------------- db statistics --------------\n");
      db->PrintStats();
      printf("-------------------------------------------\n");
    }
}

string ParseCommandLine(int argc, const char *argv[], utils::Properties &props) {
  int argindex = 1;
  string filename;
  while (argindex < argc && StrStartWith(argv[argindex], "-")) {
    if (strcmp(argv[argindex], "-threads") == 0) {
      argindex++;
      if (argindex >= argc) {
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("threadcount", argv[argindex]);
      argindex++;
    } else if(strcmp(argv[argindex],"-runtime")==0){
      argindex++;
      if(argindex >= argc){
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("runtime",argv[argindex]);
      argindex++;
    } else if(strcmp(argv[argindex],"-skip")==0){
      argindex++;
      if(argindex >= argc){
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("skip",argv[argindex]);
      argindex++;
    } else if(strcmp(argv[argindex],"-bootstrap")==0){
      argindex++;
      if(argindex >= argc){
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("bootstrap",argv[argindex]);
      argindex++;
    } else if(strcmp(argv[argindex],"-transform")==0){
      argindex++;
      if(argindex >= argc){
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("transform",argv[argindex]);
      argindex++;
    } else if(strcmp(argv[argindex],"-transformtype")==0){
      argindex++;
      if(argindex >= argc){
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("transformtype",argv[argindex]);
      argindex++;
    } else if(strcmp(argv[argindex],"-translevel")==0){
      argindex++;
      if(argindex >= argc){
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("translevel",argv[argindex]);
      argindex++;
    } else if(strcmp(argv[argindex],"-throughput")==0){
      argindex++;
      if(argindex >= argc){
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("throughput",argv[argindex]);
      argindex++;
    } else if (strcmp(argv[argindex], "-fieldcount") == 0) {
      argindex++;
      if (argindex >= argc) {
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("fieldcount", argv[argindex]);
      argindex++;
    } else if (strcmp(argv[argindex], "-levels") == 0) {
      argindex++;
      if (argindex >= argc) {
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("levels", argv[argindex]);
      argindex++;
    } else if (strcmp(argv[argindex], "-db") == 0) {
      argindex++;
      if (argindex >= argc) {
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("dbname", argv[argindex]);
      argindex++;
    } else if (strcmp(argv[argindex], "-table") == 0) {
      argindex++;
      if (argindex >= argc) {
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("table", argv[argindex]);
      argindex++;
    } else if (strcmp(argv[argindex], "-host") == 0) {
      argindex++;
      if (argindex >= argc) {
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("host", argv[argindex]);
      argindex++;
    } else if (strcmp(argv[argindex], "-port") == 0) {
      argindex++;
      if (argindex >= argc) {
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("port", argv[argindex]);
      argindex++;
    } else if (strcmp(argv[argindex], "-slaves") == 0) {
      argindex++;
      if (argindex >= argc) {
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("slaves", argv[argindex]);
      argindex++;
    } else if(strcmp(argv[argindex],"-dbpath")==0){
      argindex++;
      if (argindex >= argc) {
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("dbpath", argv[argindex]);
      argindex++;
    } else if(strcmp(argv[argindex],"-load")==0){
      argindex++;
      if(argindex >= argc){
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("load",argv[argindex]);
      argindex++;
    } else if(strcmp(argv[argindex],"-run")==0){
      argindex++;
      if(argindex >= argc){
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("run",argv[argindex]);
      argindex++;
    } else if(strcmp(argv[argindex],"-columndatatype")==0){
      argindex++;
      if(argindex >= argc){
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("columndatatype",argv[argindex]);
      argindex++;
    } else if(strcmp(argv[argindex],"-inputdataformat")==0){
      argindex++;
      if(argindex >= argc){
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("inputdataformat",argv[argindex]);
      argindex++;
    } else if(strcmp(argv[argindex],"-dboption")==0){
      argindex++;
      if(argindex >= argc){
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("dboption",argv[argindex]);
      argindex++;
    } else if(strcmp(argv[argindex],"-dbstatistics")==0){
      argindex++;
      if(argindex >= argc){
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("dbstatistics",argv[argindex]);
      argindex++;
    } else if(strcmp(argv[argindex],"-dbwaitforbalance")==0){
      argindex++;
      if(argindex >= argc){
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("dbwaitforbalance",argv[argindex]);
      argindex++;
    } else if(strcmp(argv[argindex],"-xputwindow")==0){
      argindex++;
      if(argindex >= argc){
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("xputwindow",argv[argindex]);
      argindex++;
    } else if(strcmp(argv[argindex],"-xputfile")==0){
      argindex++;
      if(argindex >= argc){
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("xputfile",argv[argindex]);
      argindex++;
    } else if(strcmp(argv[argindex],"-morerun")==0){
      argindex++;
      if(argindex >= argc){
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("morerun",argv[argindex]);
      argindex++;
    } else if(strcmp(argv[argindex],"-createdb")==0){
      argindex++;
      if(argindex >= argc){
        UsageMessage(argv[0]);
        exit(0);
      }
      props.SetProperty("createdb",argv[argindex]);
      argindex++;
    } else if (strcmp(argv[argindex], "-P") == 0) {
      argindex++;
      if (argindex >= argc) {
        UsageMessage(argv[0]);
        exit(0);
      }
      filename.assign(argv[argindex]);
      ifstream input(argv[argindex]);
      try {
        props.Load(input);
      } catch (const string &message) {
        cout << message << endl;
        exit(0);
      }
      input.close();
      argindex++;
    } else {
      cout << "Unknown option '" << argv[argindex] << "'" << endl;
      exit(0);
    }
  }

  if (argindex == 1 || argindex != argc) {
    UsageMessage(argv[0]);
    exit(0);
  }

  return filename;
}

void UsageMessage(const char *command) {
  cout << "Usage: " << command << " [options]" << endl;
  cout << "Options:" << endl;
  cout << "  -threads n: execute using n threads (default: 1)" << endl;
  cout << "  -db dbname: specify the name of the DB to use (default: basic)" << endl;
  cout << "  -P propertyfile: load properties from the given file. Multiple files can" << endl;
  cout << "                   be specified, and will be processed in the order specified" << endl;
}

inline bool StrStartWith(const char *str, const char *pre) {
  return strncmp(str, pre, strlen(pre)) == 0;
}

void Init(utils::Properties &props){
  props.SetProperty("load","false");
  props.SetProperty("run","false");
  props.SetProperty("bootstrap","true");
  props.SetProperty("transform","false");
  props.SetProperty("transformtype", "mynoop");
  props.SetProperty("translevel", "all");
  props.SetProperty("runtime", "600");
  props.SetProperty("threadcount","1");
  props.SetProperty("throughput","false");
  props.SetProperty("fieldcount","0");
  props.SetProperty("levels", "7");
  props.SetProperty("dboption","0");
  props.SetProperty("dbstatistics","false");
  props.SetProperty("dbwaitforbalance","false");
  props.SetProperty("morerun","");
  props.SetProperty("createdb", "false");
  props.SetProperty("columndatatype", "0");
  props.SetProperty("inputdataformat", "protobuf");
  props.SetProperty("xputwindow", "10");
  props.SetProperty("xputfile", "xput_stats.csv");
}

void PrintInfo(utils::Properties &props) {
  printf("---- dbname:%s  dbpath:%s ----\n", props["dbname"].c_str(), props["dbpath"].c_str());
  printf("%s", props.DebugString().c_str());
  printf("----------------------------------------\n");
  fflush(stdout);
}

void write_csv_row(const std::vector<double>& rr, const std::string& filename) {
    std::ofstream out(filename);
    if (!out) {
        throw std::runtime_error("Failed to open " + filename);
    }

    std::size_t write_size = 5000;
    if (rr.size() < write_size) {
      write_size = rr.size();
    }

    for (std::size_t i = 0; i < write_size; ++i) {
        out << rr[i];
        if (i + 1 < write_size) {
            out << ',';   // separator
        }
    }
    out << '\n';
}
