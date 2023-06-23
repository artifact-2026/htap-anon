# htap
A hybrid transactional (RocksDB), analytical (Ceph) processing key/value store that is available, consistent, and scalable. The store is logically an LSM tree mapped in the system consisted of RocksDB instances and a Ceph storage cluster. 

## Level-0 MemTable

## Ceph Storage Cluster

### Deploying a Ceph storage cluster
#### prerequirement
Ceph requires nodes to have the following, among which on a typical Linux machine only Docker is missing. 
- Python 3
- Systemd
- NTP
- LVM2
- Docker
#### To install Docker on Ubuntu:
% sudo apt-get update
% sudo apt install apt-transport-https ca-certificates curl gnupg-agent software-properties-common
% curl -fsSL https://download.docker.com/linux/ubuntu/gpg | sudo apt-key add -
% sudo add-apt-repository "deb [arch=amd64] https://download.docker.com/linux/ubuntu bionic stable"
% sudo apt update
% sudo apt install docker-ce docker-ce-cli containerd.io

#### install cephadm (as root):
% sudo apt install -y cephadm

#### bootstrap 1st node:
% sudo hostname <host-1-short-name>     // use the short name like "node0"
% sudo hostname <host-2-short-name>
% sudo hostname <host-3-short-name>
% sudo cephadm bootstrap --mon-ip *<mon-ip>*    // use the ip that starts with "10"

#### enable ceph cli: 
% sudo cephadm add-repo --release quincy
% sudo cephadm install ceph-common

#### add hosts:
% sudo ssh-copy-id -f -i /etc/ceph/ceph.pub root@*<host-2>*
% sudo ssh-copy-id -f -i /etc/ceph/ceph.pub root@*<host-3>*

#### switch to root and check ceph status
% sudo su
% ceph status

#### add hosts to the ceph cluster
% ceph orch host add *<hostname-2>* --labels _admin  // user the "short" host name

#### create osd
% sudo ceph orch apply osd --all-available-devices
% sudo ceph orch device ls

 By then the osds are likely already created. Could use the following but possibly will get an "Already created?" message
 
% sudo ceph orch daemon add osd *<host1>*:/dev/<device>   (use the "SHORT" host name, not ip)

## check with Ceph bench test
% sudo ceph osd pool create testbench 100 100
% sudo rados bench -p testbench 10 write

#### Getting CabinDB
% git clone https://github.com/hcasalet/htap.git
% cd htap
% git submodule update --init --recursive

% sudo apt-get install cmake libgflags-dev librados-dev libradospp-dev protobuf-compiler ninja-build
% sudo apt-get install libsnappy-dev zlib1g-dev libbz2-dev
% mkdir build
% cd build
% cmake -S .. -B . -G Ninja / cmake -S .. -B .
% ninja/make

#### Prepare Client
% scp /etc/ceph/ceph.conf (and client ring) root@<client_host>:/etc/ceph

#### To run: 
% ./src/test/ycsb/ycsb_test -db mycelium -dbpath /tmp/test-mycelium -P "../src/test/ycsb/workloads/test_workloada.spec" -bootstrap true -threads 1 -load true -run false -throughput false

./src/test/ycsb/ycsb_test -db rocksdb -dbpath /mydata/test_result/test-rocksdb -P "../src/test/ycsb/workloads/test_workloada.spec" -bootstrap true -threads 10 -load false -run false -throughput true -throughputtype 2 -runtime 1800

#### To rerun:
% ceph config set mon mon_allow_pool_delete true (once)
% ceph config set global osd_max_object_size 2G
% ceph osd pool delete cabindb_pool cabindb_pool --yes-i-really-really-mean-it

#### Useful commands:
% sudo systemctl stop ceph\*.service ceph\*.target (stops all daemons)
stash: 
[submodule "rocksdb-rados-env"]
        path = src/cabindb/rocksdb-rados-env
        url = https://github.com/riversand963/rocksdb-rados-env.git

#### Mount device
% sudo su
% fdisk -l   (find the disk to be mounted)
% mkfs <device_name>
% mkdir /holly
% mount <device_name> /holly
% chown -R uid:gid /holly

#### Issues:
### _SyncLocked(): Assertion `r >= 0' failed
Reason: failed when object size gets bigger. Assertion code is -27.
Solution: increase Ceph object size
