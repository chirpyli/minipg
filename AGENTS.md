# 要求

- 通过回归测试make check-world 成功
- 每次删减功能，请自动更新mydoc/CHANGE.md文档，说明删减了什么功能。

## 不可被裁剪的部分

- btree、hash索引
- 事务


## 注意事项
-  /home/postgres/works/my-github/minipg项目目录下，可以执行rm某个文件的操作，但是不能把整个项目删除，只有确认该文件可被裁剪才可以执行rm操作
- 不建议使用perl或python等脚本修改代码
- 裁剪时不用担心全库重编译


## 参考
- postgres源码：/home/postgres/works/opensource/postgres ， 该postgres源码为minipg项目裁剪前的源码，在分析minipg项目时，可以参考该postgres源码
- 在制定裁剪方案时，可参考postgres的该功能的历史提交记录，以便于对比分析，另外可以分析该功能是在哪个版本中引入的，以便于分析该功能的引入原因，去判断裁剪方案。