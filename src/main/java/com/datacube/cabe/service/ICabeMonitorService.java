package com.datacube.cabe.service;

import com.datacube.cabe.entity.SystemInformation;
import com.datacube.cabe.utils.ResponseData;

/**
 * @author CodeFarmerPK
 * @date 2023/4/10 10:02
 */
public interface ICabeMonitorService {
    ResponseData<SystemInformation> getSystemInformation();
}
