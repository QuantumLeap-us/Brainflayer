#!/bin/bash

# 确保我们在Brainflayer目录中
cd ~/Brainflayer

# 检查scrypt-jane目录是否存在
if [ ! -d "scrypt-jane" ]; then
  echo "scrypt-jane目录不存在，正在初始化子模块..."
  git submodule init
  git submodule update
fi

# 检查scrypt-jane.c文件是否存在
if [ ! -f "scrypt-jane/scrypt-jane.c" ]; then
  echo "scrypt-jane.c文件不存在，子模块可能未正确初始化"
  exit 1
fi

# 添加detect_cpu函数定义
echo "正在添加detect_cpu函数定义..."
cat >> scrypt-jane/cpu-detection.h << 'EOF'
/* CPU检测函数 */
size_t detect_cpu(void) {
    return 0; /* 返回0表示使用通用实现 */
}
EOF

# 确保在scrypt-jane.c中包含cpu-detection.h
grep -q "#include \"cpu-detection.h\"" scrypt-jane/scrypt-jane.c
if [ $? -ne 0 ]; then
  echo "正在添加cpu-detection.h的包含..."
  sed -i '1i\#include "cpu-detection.h"' scrypt-jane/scrypt-jane.c
fi

echo "修复完成，现在尝试编译..."
make clean
make

echo "如果编译成功，您应该看到brainflayer可执行文件"
ls -l brainflayer 