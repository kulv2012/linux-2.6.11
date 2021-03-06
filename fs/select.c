#define ROUND_UP(x,y) (((x)+(y)-1)/(y))
#define DEFAULT_POLLMASK (POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM)

struct poll_table_entry {
	struct file * filp;//我等在这个文件上
	wait_queue_t wait;//是这个进程在等
	wait_queue_head_t * wait_address;//等在这个队列上
};

/*一般以页为单位记录等待事件，每一个数组元素的poll_table_entry类型数据记录了一次等待"交易"
当有事件发生时，这次交易会被撤销或者说收回。每笔交易详细记录了交易双方的信息。方便追踪。
*/
struct poll_table_page {
	struct poll_table_page * next;//如果注册的fd比较多，1页4K放不下，则用此链接起来
	struct poll_table_entry * entry;//这个指针永远指向下一个要增加的交易的首地址。
	struct poll_table_entry entries[0];//柔性指针
};

#define POLL_TABLE_FULL(table) \
	((unsigned long)((table)->entry+1) > PAGE_SIZE + (unsigned long)(table))

/*
 * Ok, Peter made a complicated, but straightforward multiple_wait() function.
 * I have rewritten this, taking some shortcuts: This code may not be easy to
 * follow, but it should be free of race-conditions, and it's practical. If you
 * understand what I'm doing here, then you understand how the linux
 * sleep/wakeup mechanism works.
 *
 * Two very simple procedures, poll_wait() and poll_freewait() make all the
 * work.  poll_wait() is an inline-function defined in <linux/poll.h>,
 * as all select/poll functions have to call it to add an entry to the
 * poll table.
 */
void __pollwait(struct file *filp, wait_queue_head_t *wait_address, poll_table *p);

void poll_initwait(struct poll_wqueues *pwq)
{
/*设置函数指针.值得注意的是，__pollwait函数指针可能会在遍历fd的过程中被置空。理由如下:
当系统在遍历fd过程中，发现前面的fd已经发生了预期的事件，于是，
理所当然的，我们不必要再去设置等待事件了，因为这次循环我们肯定不会等待睡眠的，我们会直接返回.count != 0
当然了，为了保证合理性，虽然中间碰到了事件发生，我们知道不用等了，但是，我们依然应该遍历这些fd
去判断这些fd是不是有事件，算是给后面的人一个机会吧，也算是你明知自己不会等了，
就别告诉别人说我要等你__pollwait ， 而后马上又说我不等了poll_freewait。这样不人道。
*/
	init_poll_funcptr(&pwq->pt, __pollwait);
	pwq->error = 0;
	pwq->table = NULL;//初始化
}

EXPORT_SYMBOL(poll_initwait);

void poll_freewait(struct poll_wqueues *pwq)
{
	struct poll_table_page * p = pwq->table;
	while (p) {
		struct poll_table_entry * entry;
		struct poll_table_page *old;

		entry = p->entry;
		do {//每次都要整个跑一便，性能问题3
			entry--;//从后往前，一笔笔移除交易
			//把那个文件/socket和我之间的关系断开
			remove_wait_queue(entry->wait_address,&entry->wait);
			fput(entry->filp);//这是在__pollwait干的好事，怕被释放了，故增加引用计数
		} while (entry > p->entries);
		old = p;
		p = p->next;//下一个
		free_page((unsigned long) old);//归还这一页
	}
}

EXPORT_SYMBOL(poll_freewait);

//__pollwait由驱动程序调用，
//第一个参数为驱动的当前文件描述符，
//wait_address是该文件的等待队列的地址
//_p 其实就等于本函数的地址
void __pollwait(struct file *filp, wait_queue_head_t *wait_address, poll_table *_p)
{
//container_of算出_p所在的poll_wqueues的首地址，然后返回，内核中经常这么使用的
	struct poll_wqueues *p = container_of(_p, struct poll_wqueues, pt);
	struct poll_table_page *table = p->table;

	if (!table || POLL_TABLE_FULL(table)) {//如果这个满了，再申请一个
		struct poll_table_page *new_table;
		//一次一页
		new_table = (struct poll_table_page *) __get_free_page(GFP_KERNEL);
		if (!new_table) {
			p->error = -ENOMEM;
			__set_current_state(TASK_RUNNING);
			return;
		}
		//由此可知，entry指向页的poll_table_entry数组尾部
		new_table->entry = new_table->entries;
		//挂到队列的头部
		new_table->next = table;
		p->table = new_table;
		table = new_table;
	}
//下面的功能: 
//1. 在poll_table_page中增加一条记录，表示我在等着这个文件filp的这个等待队列wait_address上。
//2. 把我这次等待事件加入到设备的等待队列wait_address中，通过list_head双向链表加入
	/* Add a new entry */
	{
		struct poll_table_entry * entry = table->entry;
		table->entry = entry+1;//尾部向上增长,注意，移动到下一个结构体起始位置
	 	get_file(filp);//因为我等在你上面，所以增加其引用计数
	 	entry->filp = filp;//记录等待的文件,socket
		entry->wait_address = wait_address;//记录等在哪个等待队列上
		init_waitqueue_entry(&entry->wait, current);//是这个进程在等哈
		add_wait_queue(wait_address,&entry->wait);//将2这连接起来，将这条记录增加到wait_address的列表中
		//表示等待wait_address的进程记录
	}
/*
大概在啰嗦一下等待队列，系统在被等待者和等待者2方面都记录了这次事件，相当于2个人结婚，必定双方都有结婚证
等待者:记录了这次"交易"是等在谁上面，进程号是多少 被等待者是谁
被等待者: 记录谁在我身上等待，并且指向被等待者的对应wait_queue_t结构。
这样当有事件发生时，被等待者能够找到对应的进程，然后唤醒它。等待者也能够在适当时机有机会
撤销这次等待。比如poll_freewait就是做这样的事情，撤销这次等待。
*/
}

#define FDS_IN(fds, n)		(fds->in + n)
#define FDS_OUT(fds, n)		(fds->out + n)
#define FDS_EX(fds, n)		(fds->ex + n)

#define POLLFD_PER_PAGE  ((PAGE_SIZE-sizeof(struct poll_list)) / sizeof(struct pollfd))
//sock_poll从socket移过来的
/* No kernel lock held - perfect */
static unsigned int sock_poll(struct file *file, poll_table * wait)
{
	struct socket *sock;

	/*
	 *	We can't return errors to poll, so it's either yes or no. 
	 */
	sock = SOCKET_I(file->f_dentry->d_inode);
	return sock->ops->poll(file, sock, wait);//对于tcp，实际调用tcp_poll
}

//tcp_poll从tcp.c移动过来
/*
 *	Wait for a TCP event.
 *
 *	Note that we don't need to lock the socket, as the upper poll layers
 *	take care of normal races (between the test and the event) and we don't
 *	go look at any of the socket buffers directly.
 */
unsigned int tcp_poll(struct file *file, struct socket *sock, poll_table *wait)
{
/*poll向驱动询问其是否有事件发生了，并注册一个等待事件。
不过我纳闷啊，为何poll_wait调用的那么早 � 
如果本socket发生了事件，那就表示不需要注册事件了，因为我们待会就要返回的
*/
	unsigned int mask;
	struct sock *sk = sock->sk;
	struct tcp_sock *tp = tcp_sk(sk);

	poll_wait(file, sk->sk_sleep, wait);
	if (sk->sk_state == TCP_LISTEN)
		return tcp_listen_poll(sk, wait);

	/* Socket is not locked. We are protected from async events
	   by poll logic and correct handling of state changes
	   made by another threads is impossible in any case.
	 */

	mask = 0;
	if (sk->sk_err)
		mask = POLLERR;

	/*
	 * POLLHUP is certainly not done right. But poll() doesn't
	 * have a notion of HUP in just one direction, and for a
	 * socket the read side is more interesting.
	 *
	 * Some poll() documentation says that POLLHUP is incompatible
	 * with the POLLOUT/POLLWR flags, so somebody should check this
	 * all. But careful, it tends to be safer to return too many
	 * bits than too few, and you can easily break real applications
	 * if you don't tell them that something has hung up!
	 *
	 * Check-me.
	 *
	 * Check number 1. POLLHUP is _UNMASKABLE_ event (see UNIX98 and
	 * our fs/select.c). It means that after we received EOF,
	 * poll always returns immediately, making impossible poll() on write()
	 * in state CLOSE_WAIT. One solution is evident --- to set POLLHUP
	 * if and only if shutdown has been made in both directions.
	 * Actually, it is interesting to look how Solaris and DUX
	 * solve this dilemma. I would prefer, if PULLHUP were maskable,
	 * then we could set it on SND_SHUTDOWN. BTW examples given
	 * in Stevens' books assume exactly this behaviour, it explains
	 * why PULLHUP is incompatible with POLLOUT.	--ANK
	 *
	 * NOTE. Check for TCP_CLOSE is added. The goal is to prevent
	 * blocking on fresh not-connected or disconnected socket. --ANK
	 */
	if (sk->sk_shutdown == SHUTDOWN_MASK || sk->sk_state == TCP_CLOSE)
		mask |= POLLHUP;
	if (sk->sk_shutdown & RCV_SHUTDOWN)
		mask |= POLLIN | POLLRDNORM;
//检查是否有事件
	/* Connected? */
	if ((1 << sk->sk_state) & ~(TCPF_SYN_SENT | TCPF_SYN_RECV)) {
		/* Potential race condition. If read of tp below will
		 * escape above sk->sk_state, we can be illegally awaken
		 * in SYN_* states. */
		if ((tp->rcv_nxt != tp->copied_seq) &&
		    (tp->urg_seq != tp->copied_seq ||
		     tp->rcv_nxt != tp->copied_seq + 1 ||
		     sock_flag(sk, SOCK_URGINLINE) || !tp->urg_data))
			mask |= POLLIN | POLLRDNORM;

		if (!(sk->sk_shutdown & SEND_SHUTDOWN)) {
			if (sk_stream_wspace(sk) >= sk_stream_min_wspace(sk)) {
				mask |= POLLOUT | POLLWRNORM;
			} else {  /* send SIGIO later */
				set_bit(SOCK_ASYNC_NOSPACE,
					&sk->sk_socket->flags);
				set_bit(SOCK_NOSPACE, &sk->sk_socket->flags);

				/* Race breaker. If space is freed after
				 * wspace test but before the flags are set,
				 * IO signal will be lost.
				 */
				if (sk_stream_wspace(sk) >= sk_stream_min_wspace(sk))
					mask |= POLLOUT | POLLWRNORM;
			}
		}

		if (tp->urg_data & TCP_URG_VALID)
			mask |= POLLPRI;
	}
	return mask;
}

/*处理一页pollfd句柄，fdpage为本页的句柄结构开始地址，
//pwait是回调函数包装器的地址(指针的指针),
//count输出变化数目，累加，不能清零处理
pwait这个变量还是有点迷糊，梳理一下:
pwait一次循环后置空，表示不重复注册等待事件。
pwait在轮询过程中，对没有发生过事件的fd注册一个等待事件，
以供待会如果全都没有发生事件，则用来等待被事件唤醒。如果在轮询过程中
遇到了事件，则后续的fd都不需要注册事件了，因为反正我们不会等待了的。ps:如果此时前面的发生了事件呢 �
不过有个问题哈，在发现事件后，为啥我们不把前面的注册等待事件给取消掉???? 为了性能?因为我们反正会取消的，等退出的时候。
还是因为其他原因? 知道的同学请教一下 wuhaiwen@baidu.com

*/
static void do_pollfd(unsigned int num, struct pollfd * fdpage,
	poll_table ** pwait, int *count)
{
	int i;

	for (i = 0; i < num; i++) {//对本页的所有pollfd
		int fd;
		unsigned int mask;
		struct pollfd *fdp;

		mask = 0;
		fdp = fdpage+i;//指向当前的fd结构
		fd = fdp->fd;//句柄
		if (fd >= 0) {//以防万一吧，要是用户没有初始化···
		//下面根据句柄值，得到file结构，其实际上为socket，file,等结构，linux什么都是文件
			struct file * file = fget(fd);//增加引用计数
			mask = POLLNVAL;
			if (file != NULL) {
				mask = DEFAULT_POLLMASK;
				//如果此设备，网卡等支持等待查询poll.如果文件是socket，则poll是网卡驱动实现的
				//如果设备是文件系统，则由文件系统驱动实现。
				//下面咱们以tcp socket为例子介绍一下驱动程序实现的poll.TCP的poll成员设置在af_inet.c文件中。
				//poll字段被设置为sock_poll,后者直接调用tcp_poll,文件系统等也类似，管道调用pipe_poll，而他们的实现基本相同:调用poll_wait
				//poll_wait设备以自己的等待队列为参数，调用pwait所指向的回调函数，就是:__pollwait !!!
				if (file->f_op && file->f_op->poll)
					mask = file->f_op->poll(file, *pwait);
				mask &= fdp->events | POLLERR | POLLHUP;//用户要关注的事件+错误，断开事件，求交
				fput(file);//减少引用计数
			}
			if (mask) {//如果非0，表示发生了用户关注的事件fdp->events或POLLERR | POLLHUP
				*pwait = NULL;//将回调函数包装器指针置空，注意不是将包装器置空，是改变参数的指向
				(*count)++;//out参数记录发生了变化的事件
			}
		}
		fdp->revents = mask;//记录此文件/socket发生的事件掩码，可在应用程序中查询
	}
}

//nfds: 注册的fd总数
//list: 用户传进来的
static int do_poll(unsigned int nfds,  struct poll_list *list,
			struct poll_wqueues *wait, long timeout)
{
	int count = 0;
	poll_table* pt = &wait->pt;//记录回调函数指针

	if (!timeout)//如果等待时间为0,··，表示不等待
		pt = NULL;
 
	for (;;) {//不到黄河心不死，一直等待到timeout
		struct poll_list *walk;
		set_current_state(TASK_INTERRUPTIBLE);//可中断的等待状态，允许被中断唤醒
		walk = list;//<从头再来>
		while(walk != NULL) {//一次遍历一页，传入本页的pollfd数目，开始地址，回调函数，以及输出参数
			//pt指向参数wait的pt的地址，如果监视的文件发生了变化，则会被置空poll_table,从而不等待
			//又遍历，能不慢吗，性能问题4
			do_pollfd( walk->len, walk->entries, &pt, &count);
			walk = walk->next;
		}
		pt = NULL;//注册过一次事件了，别重复注册
		if (count || !timeout || signal_pending(current))//有事件/超时/发生中断
			break;
		count = wait->error;
		if (count)//有错
			break;
		timeout = schedule_timeout(timeout);//睡一会，然后减去睡掉的时间赋值给timeout。
		//如果中间有人叫我，因为set_current_state(TASK_INTERRUPTIBLE);，所以我会被叫醒的
	}
	__set_current_state(TASK_RUNNING);//这是啥意 ?不是本来就这样吗?谁告诉我一下hw_henry2008@126.com
	return count;
}

asmlinkage long sys_poll(struct pollfd __user * ufds, unsigned int nfds, long timeout)
{
	//后面基本可以说明，poll的缺陷的根源在于可重入，
	//每次都要做很多重复的工作，考数据，分配内存，准备数据
	//还好，时间复杂度是O(n)的
	struct poll_wqueues table;
 	int fdcount, err;
 	unsigned int i;
	struct poll_list *head;
 	struct poll_list *walk;

	/* Do a sanity check on nfds ... */
	if (nfds > current->files->max_fdset && nfds > OPEN_MAX)
		return -EINVAL;

	if (timeout) {
		/* Careful about overflow in the intermediate values */
		if ((unsigned long) timeout < MAX_SCHEDULE_TIMEOUT / HZ)
			timeout = (unsigned long)(timeout*HZ+999)/1000+1;
		else /* Negative or overflow */
			timeout = MAX_SCHEDULE_TIMEOUT;
	}

	poll_initwait(&table);//设置__pollwait函数指针到table的pt里面

	head = NULL;
	walk = NULL;
	i = nfds;
	err = -ENOMEM;
	while(i!=0) {//#我们需要每次都重复同样的工作，开辟内存，从用户空间到内核考数据。性能问题1
		struct poll_list *pp;
		pp = kmalloc(sizeof(struct poll_list)+
				sizeof(struct pollfd)*
				(i>POLLFD_PER_PAGE?POLLFD_PER_PAGE:i),
					GFP_KERNEL);
		//如果要注册的fd比较多，一页放不下，则需要循环一页页分配，组成双向链表，
		//每个链表元素的前面是list前后指针.注意poll_list是一个柔性数组，entries成员就是data的首地址
		if(pp==NULL)
			goto out_fds;
		pp->next=NULL;
		pp->len = (i>POLLFD_PER_PAGE?POLLFD_PER_PAGE:i);//记录本页有多少个用户传进来的pollfd 
		if (head == NULL)
			head = pp;
		else
			walk->next = pp;

		walk = pp;//从用户空间中拷贝pollfd 结构到内核空间，此处开销大
		if (copy_from_user(pp->entries, ufds + nfds-i, 
				sizeof(struct pollfd)*pp->len)) {
			err = -EFAULT;
			goto out_fds;
		}
		i -= pp->len;
	}
	//上面的部分拷贝用户注册的pollfd数组到内核空间，准备好链表
	fdcount = do_poll(nfds, head, &table, timeout);
	//下面进行清理工作，拷贝结果到用户空间，清空内存

	/* OK, now copy the revents fields back to user space. */
	walk = head;
	err = -EFAULT;
	//下面有需要一遍遍遍历，性能问题2
	while(walk != NULL) {//一页页的遍历直到尾部，注意这些页的顺序和传进来的ufds数组一一对应
		struct pollfd *fds = walk->entries;//从这页首地址开始。
		int j;

		for (j=0; j < walk->len; j++, ufds++) //将每一个pollfd发生的事件都写入用户空间中
			if(__put_user(fds[j].revents, &ufds->revents))
				goto out_fds;
		}
		walk = walk->next;//到下一页处理
  	}
	err = fdcount;//记录发生改变的数目
	//检查当前进程是否有信号处理，返回不为0表示有信号需要处理
	if (!fdcount && signal_pending(current))
		err = -EINTR;//如果发生改变的数目为0，且当前进程发生中断，
		//因为do_poll中set_current_state(TASK_INTERRUPTIBLE)将进程设置为可中断的等待状态了，
		//所以可能进程在等待网络事件的时候，发生了中断，这样进程提前退出等待了
out_fds:
	walk = head;
	while(walk!=NULL) {//归还内存
		struct poll_list *pp = walk->next;
		kfree(walk);
		walk = pp;
	}
	//!!!
	poll_freewait(&table);
	return err;
}
