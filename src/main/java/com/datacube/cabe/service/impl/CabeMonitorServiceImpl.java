package com.datacube.cabe.service.impl;

import com.datacube.cabe.entity.SystemInformation;
import com.datacube.cabe.service.ICabeMonitorService;
import com.datacube.cabe.utils.LinuxCMD;
import com.datacube.cabe.utils.LinuxExecutor;
import com.datacube.cabe.utils.ResponseData;
import com.datacube.cabe.utils.StatusCodes;
import jakarta.annotation.Resource;
import org.apache.commons.lang3.StringUtils;
import org.springframework.stereotype.Service;

import java.util.Arrays;
import java.util.List;

/**
 * @author CodeFarmerPK
 * @date 2023/4/10 10:02
 */
@Service
public class CabeMonitorServiceImpl implements ICabeMonitorService {
    LinuxExecutor linuxExecutor;

    @Override
    public ResponseData<SystemInformation> getSystemInformation() {
        SystemInformation systemInformation = new SystemInformation();
        systemInformation.setBcacheOpen(
                StringUtils.equals(
                        linuxExecutor.executeLinuxCommand(LinuxCMD.LinuxCommon.CHECK_BCACHE_MODULE),
                        LinuxCMD.BcacheCommon.BCACHE));
        List<String> diskHDD = Arrays.stream(
                linuxExecutor.executeLinuxCommand(LinuxCMD.LinuxCommon.FIND_HDD).split("\n")
        ).toList();
        List<String> diskSSD = Arrays.stream(
                linuxExecutor.executeLinuxCommand(LinuxCMD.LinuxCommon.FIND_SSD).split("\n")
        ).toList();
        systemInformation.setDiskHDD(diskHDD);
        systemInformation.setDiskSSD(diskSSD);
        return new ResponseData<>(StatusCodes.Response.SUCCESS, systemInformation);
    }

    @Resource
    public void setLinuxExecutor(LinuxExecutor linuxExecutor) {
        this.linuxExecutor = linuxExecutor;
    }
}
