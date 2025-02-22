#pragma once

#include <string>
#include <thread>
#include <vector>
#include "Player.h"
#include "Projectile.h"
#include "Network.h"

#define MAX_PROJECTILES 6

enum class NetworkState
{
	Offline,
	Connecting,
	Connected
};

enum class NetworkResult
{
	SUCCESS,
	FAILURE
};

class NetworkManager
{
private:

	std::string IP;
	int PORT;
	WSASession session;
	UDPSocket socket;

	unsigned int playerID;
	char sendBuffer[500];
	char recvBuffer[500];
	bool newData;
	bool running;

	std::vector<Player*> remotePlayers;

	NetworkState state = NetworkState::Offline;

	std::thread recvFromThread;

	void ReceiveFrom();

	//Data required to make remote players
	Mesh* playerMesh;
	Material* playerMat;
	std::vector<GameEntity*>* entities;

public:
	
	NetworkManager(std::vector<GameEntity*>* entityList) { entities = entityList; }

	~NetworkManager();

	NetworkState GetNetworkState() { return state; }

	NetworkResult Connect(std::string ip, int port, Player* local, Mesh* mesh, Material* mat);
	NetworkResult Disconnect();

	void CopyPlayerMovementData(Player* player, char* bff);
	void ReadPlayerMovementData(Player* player, char* buffer);

	void CopyProjectileMovementData(Projectile* projectile, char* bff);
	void ReadProjectileMovementData(Projectile* projectile, char* buffer);

	void AddNetworkProjectile(Projectile* projectile, int index);

	void Update(float dt, Player* local, Projectile** projectiles);

};

