#ifndef NEL_NETWORK_H_
#define NEL_NETWORK_H_

/* BIG TODO: test this on Windows */

#include <core/array.h>
#include <core/map.h>
#include <stdio.h>
#include <thread>
#include <condition_variable>

#if defined(_WIN32) /* on Windows */
#include <winsock2.h>
typedef int socklen_t;

#elif __APPLE__ /* on Mac */
#include <sys/event.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

#else /* on Linux */
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#endif


#define EVENT_QUEUE_CAPACITY 1024


namespace nel {

using namespace core;

struct socket_type {
#if defined(_WIN32)
	SOCKET handle;

	static constexpr SOCKET EMPTY_SOCKET = INVALID_SOCKET;

	socket_type() { }
	socket_type(const SOCKET& src) : handle(src) { }

	inline bool is_valid() const {
		return handle != INVALID_SOCKET;
	}
#else
	int handle;

	static constexpr int EMPTY_SOCKET = -1;

	socket_type() { }
	socket_type(int src) : handle(src) { }

	inline bool is_valid() const {
		return handle >= 0;
	}
#endif

	inline bool operator == (const socket_type& other) const {
		return handle == other.handle;
	}

	static inline bool is_empty(const socket_type& key) {
		return key.handle == EMPTY_SOCKET;
	}

	static inline void set_empty(socket_type& key) {
		key.handle = EMPTY_SOCKET;
	}

	static inline unsigned int hash(const socket_type& key) {
		return default_hash(key.handle);
	}

	static inline void move(const socket_type& src, socket_type& dst) {
		dst.handle = src.handle;
	}
};

void* alloc_socket_keys(size_t n, size_t element_size) {
	socket_type* keys = (socket_type*) malloc(sizeof(socket_type) * n);
	if (keys == NULL) return NULL;
	for (unsigned int i = 0; i < n; i++)
		socket_type::set_empty(keys[i]);
	return (void*) keys;
}

inline void listener_error(const char* message) {
#if defined(_WIN32)
	errno = GetLastError();
#endif
	perror(message);
}

struct socket_listener {
#if defined(_WIN32) /* on Windows */
	HANDLE listener;
	OVERLAPPED_ENTRY overlapped[EVENT_QUEUE_CAPACITY];

	template<bool Oneshot>
	inline bool add_socket(socket_type& socket) {
		if (CreateIoCompletionPort(socket.handle, listener, (DWORD) socket.handle, 0) == NULL) {
			listener_error("socket_listener.add_socket ERROR: Failed to listen to socket");
			return false;
		}
		return true;
	}

	template<bool Oneshot>
	constexpr bool update_socket(socket_type& socket) const { return true; }

	constexpr bool remove_socket(socket_type& socket) const { return true; }

	template<typename SocketReadyFunction>
	inline bool listen(SocketReadyFunction callback) {
		ULONG event_count;
		bool result = GetQueuedCompletionStatusEx(listener, overlapped, EVENT_QUEUE_CAPACITY, &event_count, INFINITE, false);
		if (!result) {
			listener_error("socket_listener.listen ERROR: Error listening for incoming network activity");
			return false;
		}

		for (ULONG i = 0; i < event_count; i++) {
			socket_type socket = (SOCKET) overlapped[i].lpCompletionKey;
			callback(socket);
		}
	}

	static inline void free(socket_listener& listener) {
		CloseHandle(listener.listener);
	}
#elif defined(__APPLE__) /* on Mac */
	int listener;
	struct kevent events[EVENT_QUEUE_CAPACITY];

	template<bool Oneshot>
	inline bool add_socket(socket_type& socket,
			const char* error_message = "socket_listener.add_socket ERROR: Failed to listen to socket")
	{
		struct kevent new_event;
		EV_SET(&new_event, socket.handle, EVFILT_READ, EV_ADD | (Oneshot ? EV_ONESHOT : 0), 0, 0, NULL);
		if (kevent(listener, &new_event, 1, NULL, 0, NULL) == -1) {
			listener_error(error_message);
			return false;
		}
		return true;
	}

	template<bool Oneshot>
	inline bool update_socket(socket_type& socket) {
		return add_socket<Oneshot>(socket, "socket_listener.update_socket ERROR: Failed to modify listen event");
	}

	constexpr bool remove_socket(socket_type& socket) const {
		return true;
	}

	template<typename SocketReadyFunction>
	inline bool listen(SocketReadyFunction callback) {
		int event_count = kevent(listener, NULL, 0, events, EVENT_QUEUE_CAPACITY, NULL);
		if (event_count == -1) {
			listener_error("socket_listener.listen ERROR: Error listening for incoming network activity");
			return false;
		}

		for (int i = 0; i < event_count; i++) {
			socket_type socket = (int) events[i].ident;
			callback(socket);
		}
		return true;
	}

	static inline void free(socket_listener& listener) {
		close(listener.listener);
	}
#else /* on Linux */
	int listener;
	epoll_event events[EVENT_QUEUE_CAPACITY];

	template<bool Oneshot>
	inline bool add_socket(socket_type& socket) {
		epoll_event new_event = {0};
		new_event.events = EPOLLIN | EPOLLERR | (Oneshot ? EPOLLONESHOT : 0);
		new_event.data.fd = socket.handle;
		if (epoll_ctl(listener, EPOLL_CTL_ADD, socket.handle, &new_event) == -1) {
			listener_error("socket_listener.add_socket ERROR: Failed to listen to socket");
			return false;
		}
		return true;
	}

	template<bool Oneshot>
	inline bool update_socket(socket_type& socket) {
		epoll_event new_event = {0};
		new_event.events = EPOLLIN | EPOLLERR | (Oneshot ? EPOLLONESHOT : 0);
		new_event.data.fd = socket.handle;
		if (epoll_ctl(listener, EPOLL_CTL_MOD, socket.handle, &new_event) == -1) {
			listener_error("socket_listener.update_socket ERROR: Failed to modify listen event");
			shutdown(socket.handle, 2); return false;
		}
		return true;
	}

	inline bool remove_socket(socket_type& socket) {
		if (epoll_ctl(listener, EPOLL_CTL_DEL, socket.handle, NULL) == -1) {
			listener_error("socket_listener.remove_socket ERROR: Failed to remove listen event");
			return false;
		}
		return true;
	}

	template<typename SocketReadyFunction>
	inline bool listen(SocketReadyFunction callback) {
		int event_count = epoll_wait(listener, events, EVENT_QUEUE_CAPACITY, -1);
		if (event_count == -1) {
			listener_error("socket_listener.listen ERROR: Error listening for incoming network activity");
			return false;
		}

		for (int i = 0; i < event_count; i++) {
			socket_type socket = events[i].data.fd;
			callback(socket);
		}
		return true;
	}

	static inline void free(socket_listener& listener) {
		close(listener.listener);
	}
#endif
};

inline bool init(socket_listener& listener) {
#if defined(_WIN32)
	listener.listener = CreateIoCompletionPort(INVALID_QUEUE_HANDLE, NULL, 0, 0);
	bool success = (listener.listener != NULL);
#elif defined(__APPLE__)
	listener.listener = kqueue();
	bool success = (listener.listener != -1);
#else
	listener.listener = epoll_create1(0);
	bool success = (listener.listener != -1);
#endif
	if (!success) {
		listener_error("init ERROR: Unable to initialize socket listener");
		return false;
	}
	return true;
}

/**
 * Reads `sizeof(T)` bytes from `in` and writes them to the memory referenced
 * by `value`. This function does not perform endianness transformations.
 * \param in a handle to a socket.
 * \tparam T satisfies [is_fundamental](http://en.cppreference.com/w/cpp/types/is_fundamental).
 */
template<typename T, typename std::enable_if<std::is_fundamental<T>::value>::type* = nullptr>
inline bool read(T& value, socket_type& in) {
	return (recv(in.handle, &value, sizeof(T), MSG_WAITALL) > 0);
}

/**
 * Reads `length` elements from `in` and writes them to the native array
 * `values`. This function does not perform endianness transformations.
 * \param in a handle to a socket.
 * \tparam T satisfies [is_fundamental](http://en.cppreference.com/w/cpp/types/is_fundamental).
 */
template<typename T, typename std::enable_if<std::is_fundamental<T>::value>::type* = nullptr>
inline bool read(T* values, socket_type& in, unsigned int length) {
	return (recv(in.handle, values, sizeof(T) * length, MSG_WAITALL) > 0);
}

/**
 * Writes `sizeof(T)` bytes to `out` from the memory referenced by `value`.
 * This function does not perform endianness transformations.
 * \param out a handle to a socket.
 * \tparam T satisfies [is_fundamental](http://en.cppreference.com/w/cpp/types/is_fundamental).
 */
template<typename T, typename std::enable_if<std::is_fundamental<T>::value>::type* = nullptr>
inline bool write(const T& value, socket_type& out) {
	return (send(out.handle, &value, sizeof(T), 0) > 0);
}

/**
 * Writes `length` elements to `out` from the native array `values`. This
 * function does not perform endianness transformations.
 * \param out a handle to a socket.
 * \tparam T satisfies [is_fundamental](http://en.cppreference.com/w/cpp/types/is_fundamental).
 */
template<typename T, typename std::enable_if<std::is_fundamental<T>::value>::type* = nullptr>
inline bool write(const T* values, socket_type& out, unsigned int length) {
	return (send(out.handle, values, sizeof(T) * length, 0) > 0);
}

inline void network_error(const char* message) {
#if defined(_WIN32)
	errno = WSAGetLastError();
#endif
	perror(message);
}

template<typename ProcessMessageCallback, typename... CallbackArgs>
void run_worker(
		array<socket_type>& event_queue, socket_listener& listener, hash_set<socket_type>& connections,
		std::condition_variable& cv, std::mutex& event_queue_lock, std::mutex& connection_set_lock,
		bool& server_running, ProcessMessageCallback process_message, CallbackArgs&&... callback_args)
{
	while (server_running) {
		std::unique_lock<std::mutex> lck(event_queue_lock);
		cv.wait(lck);
		if (!server_running) break;
		socket_type connection = event_queue.pop();
		lck.unlock();

		uint8_t next;
		if (recv(connection.handle, &next, sizeof(next), MSG_PEEK) == 0) {
			/* the other end of the socket was closed by the client */
			listener.remove_socket(connection);
			connection_set_lock.lock();
			connections.remove(connection);
			connection_set_lock.unlock();
			shutdown(connection.handle, 2);
		} else {
			/* there is a data waiting to be read, so read it */
			process_message(connection, std::forward<CallbackArgs>(callback_args)...);

			/* tell epoll to continue polling this socket */
			if (!listener.update_socket<true>(connection))
				shutdown(connection.handle, 2);
		}
	}
}

template<bool Success>
inline void cleanup_server(
		bool& server_running, std::condition_variable& init_cv)
{
#if defined(_WIN32)
	WSACleanup();
#endif
	if (!Success) {
		server_running = false;
		init_cv.notify_all();
	}
}

template<bool Success>
inline void cleanup_server(
		bool& server_running, std::condition_variable& init_cv, socket_type& sock)
{
	shutdown(sock.handle, 2);
	cleanup_server<Success>(server_running, init_cv);
}

template<bool Success>
inline void cleanup_server(
		bool& server_running, std::condition_variable& init_cv,
		socket_type& sock, socket_listener& listener)
{
	core::free(listener);
	cleanup_server<Success>(server_running, init_cv, sock);
}

template<typename ProcessMessageCallback, typename... CallbackArgs>
bool run_server(socket_type& sock, uint16_t server_port,
		unsigned int connection_queue_capacity, unsigned int worker_count,
		bool& server_running, std::condition_variable& init_cv,
		ProcessMessageCallback process_message, CallbackArgs&&... callback_args)
{
#if defined(_WIN32)
	WSADATA wsa_state;
	if (WSAStartup(MAKEWORD(2,2), &wsa_state) != NO_ERROR) {
		fprintf(stderr, "run_server ERROR: Unable to initialize WinSock.\n");
		server_running = false; init_cv.notify_all(); return false;
	}
#endif

	sock = socket(AF_INET6, SOCK_STREAM, 0);
	if (!sock.is_valid()) {
		network_error("run_server ERROR: Unable to open socket");
		cleanup_server<false>(server_running, init_cv); return false;
	}

	int yes = 1;
	if (setsockopt(sock.handle, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0) {
		network_error("run_server ERROR: Unable to set socket option");
		cleanup_server<false>(server_running, init_cv, sock); return false;
	}

	sockaddr_in6 server_addr;
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin6_family = AF_INET6;
	server_addr.sin6_port = htons(server_port);
	server_addr.sin6_addr = in6addr_any;
	if (bind(sock.handle, (sockaddr*) &server_addr, sizeof(server_addr)) != 0) {
		network_error("run_server ERROR: Unable to bind to socket");
		cleanup_server<false>(server_running, init_cv, sock); return false;
	}

	if (listen(sock.handle, connection_queue_capacity) != 0) {
		network_error("run_server ERROR: Unable to listen to socket");
		cleanup_server<false>(server_running, init_cv, sock); return false;
	}

	socket_listener listener;
	if (!init(listener)) {
		network_error("run_server ERROR: Failed to initialize socket listener");
		cleanup_server<false>(server_running, init_cv, sock); return false;
	}

	if (!listener.add_socket<false>(sock)) {
		cleanup_server<false>(server_running, init_cv, sock, listener); return false;
	}

	/* make the thread pool */
	std::condition_variable cv;
	std::mutex event_queue_lock, connection_set_lock;
	hash_set<socket_type> connections(1024, alloc_socket_keys);
	array<socket_type> event_queue(64);
	std::thread* workers = new std::thread[worker_count];
	auto start_worker = [&]() {
		run_worker(event_queue, listener, connections, cv, event_queue_lock, connection_set_lock,
				server_running, process_message, std::forward<CallbackArgs>(callback_args)...);
	};
	for (unsigned int i = 0; i < worker_count; i++)
		workers[i] = std::thread(start_worker);

	/* notify that the server has successfully started */
	init_cv.notify_all();

	/* the main loop */
	sockaddr_storage client_address;
	socklen_t address_size = sizeof(client_address);
	while (server_running) {
		listener.listen([&](socket_type& socket) {
			if (socket == sock.handle) {
				/* there's a new connection on the server socket */
				socket_type connection = accept(sock.handle, (sockaddr*) &client_address, &address_size);
				if (!connection.is_valid()) {
					network_error("run_server ERROR: Error establishing connection with client");
					return;
				}

				if (!listener.add_socket<true>(connection)) {
					shutdown(connection.handle, 2); return;
				}
				connection_set_lock.lock();
				connections.add(connection, alloc_socket_keys);
				connection_set_lock.unlock();
			} else {
				/* there is an event on a client connection */
				event_queue_lock.lock();
				event_queue.add(socket);
				event_queue_lock.unlock(); cv.notify_one();
			}
		});
	}

	cv.notify_all();
	for (unsigned int i = 0; i < worker_count; i++)
		workers[i].join();
	delete[] workers;

	for (socket_type& connection : connections)
		shutdown(connection.handle, 2);
	cleanup_server<true>(server_running, init_cv, sock, listener);
	return true;
}

template<typename ProcessConnectionCallback>
bool run_client(
		const char* server_address, const char* server_port,
		ProcessConnectionCallback process_connection)
{
	addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	addrinfo* addresses;
	int result = getaddrinfo(server_address, server_port, &hints, &addresses);
	if (result != 0 || addresses == NULL) {
		fprintf(stderr, "run_client ERROR: Unable to resolve address. %s\n", gai_strerror(result));
		return false;
	}

	socket_type sock;
	for (addrinfo* entry = addresses; entry != NULL; entry++) {
		sock = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
		if (!sock.is_valid()) {
			network_error("run_client ERROR: Unable to open socket");
			continue;
		}

		if (connect(sock.handle, entry->ai_addr, entry->ai_addrlen) != 0) {
			network_error("run_client ERROR: Unable to connect");
			shutdown(sock.handle, 2); continue;
		}
		break;
	}
	freeaddrinfo(addresses);

	process_connection(sock);
	return true;
}

} /* namespace nel */

#endif /* NEL_NETWORK_H_ */
