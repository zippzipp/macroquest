#ifndef UG_PACKET
#define UG_PACKET
#include <Windows.h>
#define P_SEND_PACKET 0x140547EF0


extern "C" typedef void(__fastcall* SendPacketFunction)(uintptr_t, int, char*, size_t);
SendPacketFunction fnSend = reinterpret_cast<SendPacketFunction>(FixEQGameOffset(P_SEND_PACKET));

class UGPacket {
public:
	bool Send(unsigned int opcode, void* buff, uint32_t size) {
		char* pktBuffer = (char*)malloc(sizeof(char) * (size + 2));
		if (!pktBuffer) return false;

		uint16_t pktOpcode = static_cast<uint16_t>(opcode);
		size_t pktSize = size + 2;
		uintptr_t thisptr = *reinterpret_cast<uintptr_t*>(__gWorld);

		memset(pktBuffer, 0, sizeof(char) * (size + 2));
		memcpy(pktBuffer, &pktOpcode, sizeof(char)*2);
		memcpy(pktBuffer + 2, buff, size);

		fnSend(thisptr, 4, pktBuffer, pktSize);

		free(pktBuffer);
		return true;
	}
};

#endif // !UG_PACKET

