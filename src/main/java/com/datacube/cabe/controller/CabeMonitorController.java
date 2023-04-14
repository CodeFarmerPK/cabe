package com.datacube.cabe.controller;

import com.datacube.cabe.entity.SystemInformation;
import com.datacube.cabe.service.ICabeMonitorService;
import com.datacube.cabe.utils.ResponseData;
import jakarta.annotation.Resource;
import org.springframework.stereotype.Controller;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.ResponseBody;

/**
 * @author CodeFarmerPK
 * @date 2023/4/10 9:53
 */
@Controller
public class CabeMonitorController extends CabeBaseController {
    ICabeMonitorService iCabeMonitorService;

    /**
     * Get System Information
     */
    @GetMapping("/systemInfo")
    @ResponseBody
    public ResponseData<SystemInformation> getSystemInformation() {
        return iCabeMonitorService.getSystemInformation();
    }

    @Resource
    public void setCabeMonitorService(ICabeMonitorService iCabeMonitorService) {
        this.iCabeMonitorService = iCabeMonitorService;
    }
}
