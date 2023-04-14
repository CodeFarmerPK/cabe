package com.datacube.cabe.utils;

/**
 * @author CodeFarmerPK
 * @date 2023/4/10 11:15
 */
public interface LinuxCMD {
    interface LinuxCommon {
        String CHECK_BCACHE_MODULE = "lsmod | grep bcache -A 0 | tail -n 1 | awk '{print $1}'";
        String FIND_HDD = "cat /proc/partitions | sed '1,2d' |awk '{print \"/dev/\"$4}' |grep -v '[0-9]' | sort";
        String FIND_SSD = "nvme list | sed '1,2d' | awk '{print $1}' | sort";
    }

    interface BcacheCommon {
        String BCACHE = "bcache";
    }
}
