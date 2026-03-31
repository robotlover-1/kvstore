# KVStore RESP + TTL + 持久化 测试手册

## 启动

``` bash
./kvstore 5000
```

默认地址：

    127.0.0.1:5000

------------------------------------------------------------------------

## 1. 基本功能

### SET

``` bash
printf '*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$5\r\nalice\r\n' | nc -q 1 127.0.0.1 5000
```

**Expected**

    +OK

------------------------------------------------------------------------

### GET

``` bash
printf '*2\r\n$3\r\nGET\r\n$4\r\nname\r\n' | nc -q 1 127.0.0.1 5000
```

**Expected**

    $5
    alice

------------------------------------------------------------------------

## 2. Pipeline

``` bash
printf '*3\r\n$3\r\nSET\r\n$4\r\npkey\r\n$4\r\npval\r\n*2\r\n$3\r\nGET\r\n$4\r\npkey\r\n' | nc -q 1 127.0.0.1 5000
```

**Expected**

    +OK
    $4
    pval

------------------------------------------------------------------------

## 3. 半包

``` bash
exec 3<>/dev/tcp/127.0.0.1/5000
printf '*3\r\n$3\r\nSE' >&3
sleep 1
printf 'T\r\n$4\r\nname\r\n$5\r\nalice\r\n' >&3
cat <&3
```

**Expected**

    +OK

------------------------------------------------------------------------

## 4. 错误恢复

``` bash
printf '*2\r\n$3\r\nGET\r\n$X\r\nname\r\n*2\r\n$3\r\nGET\r\n$4\r\nname\r\n' | nc -q 1 127.0.0.1 5000
```

**Expected**

    -ERR ...
    $5
    alice

------------------------------------------------------------------------

## 5. TTL

### 设置过期

``` bash
printf '*3\r\n$6\r\nEXPIRE\r\n$4\r\nname\r\n$1\r\n5\r\n' | nc -q 1 127.0.0.1 5000
```

### 查询 TTL

``` bash
printf '*2\r\n$3\r\nTTL\r\n$4\r\nname\r\n' | nc -q 1 127.0.0.1 5000
```

**Expected**

    :5

### 过期验证

``` bash
sleep 6
printf '*2\r\n$3\r\nGET\r\n$4\r\nname\r\n' | nc -q 1 127.0.0.1 5000
```

**Expected**

    $-1

------------------------------------------------------------------------

## 6. SAVE（全量持久化）

``` bash
printf '*1\r\n$4\r\nSAVE\r\n' | nc -q 1 127.0.0.1 5000
ls -l kvstore.dump
```

------------------------------------------------------------------------

## 7. AOF（增量持久化）

``` bash
printf '*3\r\n$3\r\nSET\r\n$2\r\na1\r\n$2\r\nv1\r\n' | nc -q 1 127.0.0.1 5000
cat kvstore.aof
```

------------------------------------------------------------------------

## 8. 恢复测试

``` bash
pkill kvstore
./kvstore 5000
printf '*2\r\n$3\r\nGET\r\n$2\r\na1\r\n' | nc -q 1 127.0.0.1 5000
```

------------------------------------------------------------------------

## 9. TTL + 持久化

``` bash
printf '*3\r\n$3\r\nSET\r\n$2\r\nt1\r\n$2\r\nv1\r\n' | nc -q 1 127.0.0.1 5000
printf '*3\r\n$6\r\nEXPIRE\r\n$2\r\nt1\r\n$2\r\n20\r\n' | nc -q 1 127.0.0.1 5000

pkill kvstore
./kvstore 5000

printf '*2\r\n$3\r\nTTL\r\n$2\r\nt1\r\n' | nc -q 1 127.0.0.1 5000
```
