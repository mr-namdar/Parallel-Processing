# Task 4 – Predictions

**1. TAS Lock**

I expect TAS to work well with a small number of threads, but its speedup will probably become worse when more threads are added. This is because all threads keep trying to get the lock. I expect e(N) to increase as the number of threads grows. I also think fairness may be poor because some threads may get the lock more often than others.

**2. TTAS Lock**

I expect TTAS to perform better than TAS because threads check the lock before trying to acquire it. This may reduce unnecessary attempts. I think its speedup will increase more smoothly, and e(N) will be lower than TAS in most cases. However, fairness is not guaranteed because threads do not have a fixed order for getting the lock.

**3. TTAS with Backoff**

I think TTAS with Backoff will probably be the best spinlock when the number of threads is high. The waiting delay may reduce competition between threads. I expect better speedup and a smaller increase in e(N) compared with normal TAS and TTAS. However, some threads may wait longer than necessary because of the delay. Fairness may still be a problem.

**4. Ticket Lock**

I expect Ticket Lock to have good fairness because threads get the lock in the order they arrive. I also think it may provide stable speedup when the number of threads increases. Its e(N) may increase, but I do not expect a very large increase because threads have an organized waiting order. One possible problem is that all threads must wait for their turn.

**5. pthread_mutex**

I expect pthread_mutex to be slower than spinlocks when there are only a few threads because it has additional overhead. However, when the number of threads becomes higher, I think it may perform better because waiting threads do not need to keep using the CPU. At N = 2H, I expect the mutex may have better speedup than spinlocks. Its context switches may be higher, but I think its fairness will be reasonable.

**Overall Prediction**

I expect TTAS and TTAS with Backoff to perform better than TAS. Ticket Lock should provide better fairness, and pthread_mutex may become more useful when there are more threads than available hardware threads. I also expect F = 0.1 to have better speedup than F = 0.5 because less work is done inside the critical section.