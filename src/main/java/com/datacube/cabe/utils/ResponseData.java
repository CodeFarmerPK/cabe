package com.datacube.cabe.utils;

/**
 * @author CodeFarmerPK
 * @date 2023/4/10 10:37
 */
public class ResponseData<T> {
    private Integer code;
    private String msg;
    private T data;

    private ResponseData() {
    }

    public ResponseData(Integer code, T data) {
        this.code = code;
        this.data = data;
    }

    public ResponseData(Integer code, String msg, T data) {
        this.code = code;
        this.msg = msg;
        this.data = data;
    }

    public Integer getCode() {
        return code;
    }

    public void setCode(Integer code) {
        this.code = code;
    }

    public String getMsg() {
        return msg;
    }

    public void setMsg(String msg) {
        this.msg = msg;
    }

    public T getData() {
        return data;
    }

    public void setData(T data) {
        this.data = data;
    }
}
