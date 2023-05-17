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
% sudo cephadm bootstrap --mon-ip *<mon-ip>* --allow-fqdn-hostname

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
% ceph orch host add *<hostname-2>* --labels _admin

#### create osd
% sudo ceph orch apply osd --all-available-devices
% sudo ceph orch device ls

 By then the osds are likely already created. Could use the following but possibly will get an "Already created?" message
 
% sudo ceph orch daemon add osd *<host1>*:/dev/<device>   (use the host name, not ip)

## check with Ceph bench test
% sudo ceph osd pool create testbench 100 100
% sudo rados bench -p testbench 10 write

#### Getting CabinDB
% git clone https://github.com/hcasalet/htap.git
% cd htap
% git submodule update --init --recursive

#### To build:
% sudo apt-get install cmake libgflags-dev
% sudo apt-get install librados-dev libradospp-dev
% mkdir build
% cd build
% cmake -S .. -B .
% make

#### To run: 
% ./src/test/ycsb/ycsb_test -db cabindb -dbpath /tmp/test-cabindb -P "../src/test/ycsb/workloads/test_workloada.spec" -threads 1 -fieldcount 0 -load true -run false -throughput false

#### To rerun:
% ceph config set mon mon_allow_pool_delete true (once)
% ceph config set global osd_max_object_size 2G
% ceph osd pool delete cabindb_pool cabindb_pool --yes-i-really-really-mean-it
