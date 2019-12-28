enum {
	OpenMax = 20,
	DatagramMaxSize = 9216,
	PeersMax = OpenMax - 2, /* 1 listener + 1 datagram */
	MessageMaxSize = DatagramMaxSize - 16, /* 8 random + 8 md5 */
	MessageHistory = 128,
	ReportSize = 2 + (MessageHistory >> 4), /* seq, bitmask */
	ReportCount = (ReportSize - 2) << 3,
	HeaderSize = 6 + ReportSize + 2 * PeersMax,
	MessageDataMaxSize = MessageMaxSize - HeaderSize,
	PeerMaxSend = MessageDataMaxSize,
	PeerRecvBufSize = 5 << 19,
	MoveSize = 3 << 19,
	AlertSize = PeerRecvBufSize - (MessageHistory+1) * PeerMaxSend,
	TimeDiffMax = 300,
	SendFrequency = 40, /* minimum possible value is 2 */
	ResetAfter = 60 * SendFrequency
};

enum Msg {
	Msg_Bad = 0,
	Msg_OK = 1,
	Msg_Reset = 2,
	Msg_Reset_OK = 3
};

struct peer {
	char		 free;
	char		 dontsend;
	int		 s;
	struct {
		char		 open;
		char		 close;
		ssize_t		 off;
		ssize_t		 size;
		uint8_t		 buf[MoveSize + PeerRecvBufSize];
	} recv;
	struct {
		char		 open;
		char		 close;
		ssize_t		 size;
		uint8_t		 buf[PeerMaxSend];
	} send;
};

/* msg_recv: save the incomming message in history */
enum Msg	 msg_recv(int);

/* msg_process: process next received message in order */
int		 msg_process(struct peer *);

/* msg_send: send a new message */
void		 msg_send(int, struct peer *, const struct addrinfo *);

/* msg_sendreset: send a reset request */
void		 msg_sendreset(int, enum Msg, const struct addrinfo *);

/* msg_resendold: resend an old message if needed */
int		 msg_resendold(int, const struct addrinfo *);

/* msg_reset: reset all data */
void		 msg_reset(struct peer *);

/* msg_gettimeout: calculate timeout according to SendFrequency */
struct timeval	*msg_gettimeout(struct timeval *);

extern const struct addrinfo
	client_ai, server_ai, listen_ai, connect_ai;
