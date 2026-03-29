# RESP + TTL nc 测试手册

默认服务地址：

    127.0.0.1:5000

启动：

    ./kvstore 5000

------------------------------------------------------------------------

## 1. SET

    printf '*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$5\r\nalice\r\n' | nc -q 1 127.0.0.1 5000

**Expected:**

    +OK

------------------------------------------------------------------------

## 2. GET

    printf '*2\r\n$3\r\nGET\r\n$4\r\nname\r\n' | nc -q 1 127.0.0.1 5000

**Expected:**

    $5
    alice

------------------------------------------------------------------------

## 3. PIPELINE

    printf '*3\r\n$3\r\nSET\r\n$4\r\npkey\r\n$4\r\npval\r\n*2\r\n$3\r\nGET\r\n$4\r\npkey\r\n' | nc -q 1 127.0.0.1 5000

**Expected:**

    +OK
    $4
    pval

------------------------------------------------------------------------

## 4. EXPIRE

    printf '*3\r\n$6\r\nEXPIRE\r\n$4\r\nname\r\n$1\r\n5\r\n' | nc -q 1 127.0.0.1 5000

**Expected:**

    +OK

------------------------------------------------------------------------

## 5. TTL

    printf '*2\r\n$3\r\nTTL\r\n$4\r\nname\r\n' | nc -q 1 127.0.0.1 5000

**Expected:**

    :5

------------------------------------------------------------------------

## 6. HALF PACKET

    exec 3<>/dev/tcp/127.0.0.1/5000
    printf '*3\r\n$3\r\nSE' >&3
    sleep 1
    printf 'T\r\n$4\r\nname\r\n$5\r\nalice\r\n' >&3
    cat <&3

**Expected:**

    +OK

------------------------------------------------------------------------

## 7. ERROR RECOVERY

    printf '*2\r\n$3\r\nGET\r\n$X\r\nname\r\n*2\r\n$3\r\nGET\r\n$4\r\nname\r\n' | nc -q 1 127.0.0.1 5000

**Expected:**

    -ERR ...
    $5
    alice

------------------------------------------------------------------------

## 8. TTL EXPIRE RESULT

    sleep 6
    printf '*2\r\n$3\r\nGET\r\n$4\r\nname\r\n' | nc -q 1 127.0.0.1 5000

**Expected:**

    $-1
