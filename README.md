#组织一下这个所谓的负载均衡服务器的架构

首先是log类 这个属于是工具类 主要是 如果有什么问题出现 可以调用这个工具类将出错的信息或者进行的操作打印出来

然后是fdwrapper类 这个是将对fd的操作封装起来 这个底层其实就是用epoll来管理 然后可以将目标描述符

可以是客户端也可以是服务器端的socketfd在epoll中的加入以及删除操作 也没什么麻烦的

在之后是一个抽象出来的类 这个类用来表示服务器端与客户端的连接 其中包括两端的socket 以及两端的缓冲区 

通过对这两个缓冲区 的index进行操作 来记录读写的位置 并通过recv和send来向各自发送信息

然后就是mgr类 这个类主要是用于 一些 对conn连接的实体类的操作 包括连接到server 断开连接 将端口释放

主要用了map来进行fd和连接的映射 并可以将fd循环利用
