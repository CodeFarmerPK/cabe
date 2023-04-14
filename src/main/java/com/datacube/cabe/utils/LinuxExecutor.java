package com.datacube.cabe.utils;

import org.springframework.stereotype.Service;

import java.io.BufferedReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;

/**
 * @author CodeFarmerPK
 * @date 2023/4/10 10:35
 */
@Service
public class LinuxExecutor {

    public String executeLinuxCommand(String command) {
        String[] cmd = {"/bin/sh", "-c", command};
        StringBuilder result  = new StringBuilder();
        try {
            Process process = Runtime.getRuntime().exec(cmd);
            process.waitFor();
            InputStream ins = process.getInputStream();
            BufferedReader read = new BufferedReader(new InputStreamReader(ins));
            String line;
            while ((line = read.readLine()) != null) {
                result.append(line).append("\n");
            }
        } catch (IOException | InterruptedException e) {
            e.printStackTrace();
            return "";
        }

        return result.toString().trim();
    }
}
