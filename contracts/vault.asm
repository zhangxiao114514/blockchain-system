; vault.asm — 示例合约「累计存款」
; 语义：每次调用把 calldata[4:36] 累加到存储槽 0，并发出 Deposit 事件
;
; 事件签名 Deposit(address,uint256)，topic0 = sha256(签名) = 0fe7d01deea0603b2f9c189c2b9174b25df1fda1085b50a9bad40d29e7c1d9ff
;
; 指令序列（十六进制）：
;   600054       PUSH1 0 / SLOAD           读取累计值
;   600435       PUSH1 4 / CALLDATALOAD    读取参数（跳过 4 字节选择器）
;   01           ADD                       累加
;   80           DUP1                      复制一份用于事件
;   600055       PUSH1 0 / SSTORE          写回槽位 0
;   600052       PUSH1 0 / MSTORE          写入内存供 LOG 使用
;   7f<topic>    PUSH32 topic
;   6020         PUSH1 32                  数据长度 32 字节
;   6000         PUSH1 0                   数据偏移 0
;   a1           LOG1                      发出事件
;   00           STOP

60005460043501806000556000527f0fe7d01deea0603b2f9c189c2b9174b25df1fda1085b50a9bad40d29e7c1d9ff60206000a100
