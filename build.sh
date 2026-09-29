if [ "$1" = "clean" ]; then
    make maintainer-clean
    exit 0
fi

make maintainer-clean

# 清掉上一次 configure 残留，避免 config.status --recheck 复用旧选项
rm -f config.status config.cache config.log

./configure --prefix=/home/postgres/minipg --enable-debug --enable-cassert  --enable-depend CFLAGS="-DLOCK_DEBUG -DOPTIMIZER_DEBUG"
