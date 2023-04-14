package com.datacube.cabe.entity;

import java.util.List;

/**
 * @author CodeFarmerPK
 * @date 2023/4/10 10:07
 */
public class SystemInformation {
    Boolean bcacheOpen;
    List<String> diskHDD;
    List<String> diskSSD;

    public SystemInformation() {
    }

    public SystemInformation(Boolean bcacheOpen, List<String> diskHDD, List<String> diskSSD) {
        this.bcacheOpen = bcacheOpen;
        this.diskHDD = diskHDD;
        this.diskSSD = diskSSD;
    }

    public Boolean getBcacheOpen() {
        return bcacheOpen;
    }

    public void setBcacheOpen(Boolean bcacheOpen) {
        this.bcacheOpen = bcacheOpen;
    }

    public List<String> getDiskHDD() {
        return diskHDD;
    }

    public void setDiskHDD(List<String> diskHDD) {
        this.diskHDD = diskHDD;
    }

    public List<String> getDiskSSD() {
        return diskSSD;
    }

    public void setDiskSSD(List<String> diskSSD) {
        this.diskSSD = diskSSD;
    }
}
