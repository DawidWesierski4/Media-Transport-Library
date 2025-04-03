   1    0     1.00   1.96    3.33      10 K     65 K    0.69    0.95  0.0000  0.0000      112       17        1     21
   2    0     1.00   1.96    3.33      20 K     71 K    0.66    0.95  0.0000  0.0000      280       22        1     21
   3    0     1.00   1.48    3.33      23 K     67 K    0.61    0.94  0.0000  0.0000      504       38        0     23

CPUS="0,1,2,3,4,5,56,57,58,59,60,61"
echo sudo grubby --update-kernel=ALL --remove-args="isolcpus=${CPUS}"
sudo grubby --update-kernel=ALL --args="isolcpus=${CPUS}"

echo sudo grubby --update-kernel=ALL --remove-args="nohz_full=${CPUS}"
sudo grubby --update-kernel=ALL --args="nohz_full=${CPUS}"

echo sudo grubby --update-kernel=ALL --remove-args="rcu_nocbs=${CPUS}"
sudo grubby --update-kernel=ALL --args="rcu_nocbs=${CPUS}"


echo sudo grubby --update-kernel=ALL --remove-args="isolcpus=${CPUS}"
sudo grubby --update-kernel=ALL --args="isolcpus=${CPUS}"

echo sudo grubby --update-kernel=ALL --remove-args="nohz_full=${CPUS}"
sudo grubby --update-kernel=ALL --args="nohz_full=${CPUS}"

echo sudo grubby --update-kernel=ALL --remove-args="rcu_nocbs=${CPUS}"
sudo grubby --update-kernel=ALL --args="rcu_nocbs=${CPUS}"