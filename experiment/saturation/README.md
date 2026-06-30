### What is it? 
 ## Tools in this directory characterize a workload by identifying the resource it is going to first bottlenecked 
at when it reaches the maximum throughput, as well as measuring the slack of the other resources then.

### Identifying bottleneck
#### Tool to run: saturation_sweep.sh

#### How to run:
 1. Get the device name: 
  % iostat -dx -N -t -y 1 3
 2. Find the maximum disk IO bandwidth
  % fio --name=mixedrw \
    --filename=/data/htap/build/fio_tmp \
    --rw=rw --bs=128k --size=8G \
    --direct=1 --numjobs=4 --group_reporting
 3. Sum the numbers at bw at the bottom of what's returned.
 4. Command to issue: 
  % cd <build_dir>
  % DISK_DEVICE=sdb DISK_IO_MAX_BANDWIDTH=349 WORKLOAD_SPEC=../src/test/ycsb/workloads/test_workloada.spec WORKLOAD_LABEL="c6525-25g-sata-2k (Workload A)" THREAD_COUNTS="1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 20 24 28 32" bash ../utility/sands/saturation_sweep.sh
