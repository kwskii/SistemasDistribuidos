//Grupo 51: Felipe Heliszkowski 47064
//Gon�alo Cardoso 46784
//Pedro Gama 47081

/*
   Programa que implementa um servidor de uma tabela hash com chainning.
   Uso: table-server <port> <table1_size> [<table2_size> ...]
   Exemplo de uso: ./table_server 54321 10 15 20 25
*/
#define NFDESC 10
#define TIMEOUT 50
#include <error.h>
#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include "inet.h"
#include "table_skel.h"
#include "client_stub.h"
#include "table-private.h"
#include "message.h"
#include "primary_backup.h"
#include "network_client-private.h"

#define PRIMARY 1
#define SECONDARY 2
#define ALIVE 1
#define DEAD 0
#define BILLION 1E9

/**************GLOBALS********************/

// PRIMARY 1
// SECONDARY 2

int type;

char *port;

int updated;

struct server_t *primary;
struct server_t *secondary;

struct thread_parameters
{
	struct message_t *msg_p;
};

///	Exemplo de configS.ini
/// ip:porta				// ip:porta do servidor primario para o secundario se ligar

FILE *configP = NULL, *configS = NULL;

// TO-DO MUTEX'S

int locked = 0;

pthread_mutex_t dados = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t dados_disponiveis = PTHREAD_COND_INITIALIZER;

/******************************************/

void print_message(struct message_t *msg)
{
	int i;

	printf("\n----- MESSAGE -----\n");
	printf("Tabela n�mero: %d\n", msg->table_num);
	printf("opcode: %d, c_type: %d\n", msg->opcode, msg->c_type);
	switch (msg->c_type)
	{
	case CT_ENTRY:
	{
		printf("key: %s\n", msg->content.entry->key);
		printf("datasize: %d\n", msg->content.entry->value->datasize);
	}
	break;
	case CT_KEY:
	{
		printf("key: %s\n", msg->content.key);
	}
	break;
	case CT_KEYS:
	{
		for (i = 0; msg->content.keys[i] != NULL; i++)
		{
			printf("key[%d]: %s\n", i, msg->content.keys[i]);
		}
	}
	break;
	case CT_VALUE:
	{
		if (msg->content.data != NULL)
			printf("datasize: %d\n", msg->content.data->datasize);
	}
	break;
	case CT_RESULT:
	{
		printf("result: %d\n", msg->content.result);
	}
	break;
	case OC_RT_ERROR:
	{
		printf("result: %d\n", msg->content.result);
	};
	}
	printf("-------------------\n");
}

/* Fun��o para preparar uma socket de rece��o de pedidos de liga��o.
*/
int make_server_socket(short port)
{
	int socket_fd;
	struct sockaddr_in server;

	if ((socket_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		perror("Erro ao criar socket");
		return -1;
	}

	server.sin_family = AF_INET;
	server.sin_port = htons(port);
	server.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(socket_fd, (struct sockaddr *)&server, sizeof(server)) < 0)
	{
		perror("Erro ao fazer bind");
		close(socket_fd);
		return -1;
	}

	if (listen(socket_fd, 1) < 0)
	{
		perror("Erro ao executar listen");
		close(socket_fd);
		return -1;
	}
	return socket_fd;
}

/* 
Thread onde corre a comunicacao de mensagens entre os servidores
*/
void *comm(void *params)
{
	struct thread_parameters *tp = (struct thread_parameters *)params;
	struct message_t *msg_r;
	int *result = 0;
	result = (int *)malloc(sizeof(int));

	msg_r = network_send_receive(primary->secondary, tp->msg_p);
	if (msg_r == NULL)
	{
		printf("Erro no network_send_receive da thread\n");
		*result = -1;
		return result;
	}

	*result = msg_r->content.result;
	free_message(msg_r);
	return result;
}

/* Fun��o que recebe uma tabela e uma mensagem de pedido e:
	- aplica a opera��o na mensagem de pedido na tabela;
	- devolve uma mensagem de resposta com oresultado.
*/
struct message_t *process_message(struct message_t *msg_p, struct table_t *tabela)
{
	struct message_t *msg_r;

	/* Verificar par�metros de entrada */

	if (msg_p == NULL)
		return NULL;
	if (tabela == NULL)
		return NULL;

	/* Verificar opcode e c_type na mensagem de pedido */
	if (!(msg_p->opcode == OC_SIZE || msg_p->opcode == OC_COLLS || msg_p->opcode == OC_UPDATE || msg_p->opcode == OC_GET || msg_p->opcode == OC_PUT) ||
		!(msg_p->c_type == CT_RESULT || msg_p->c_type == CT_VALUE || msg_p->c_type == CT_KEY || msg_p->c_type == CT_KEYS || msg_p->c_type == CT_ENTRY))
		return NULL;

	int result = 0;
	struct data_t *data = (struct data_t *)malloc(sizeof(struct data_t));
	char **keys;
	int dataOrkeys = 0;

	/* Aplicar opera��o na tabela */
	if (msg_p->opcode == OC_PUT)
	{
		result = table_put(tabela, msg_p->content.entry->key, msg_p->content.entry->value);
	}
	else if (msg_p->opcode == OC_GET)
	{
		if (strcmp(msg_p->content.key, "*") != 0) //Caso normal
			data = table_get(tabela, msg_p->content.key);
		else
		{ // Pega tudo
			keys = table_get_keys(tabela);
			dataOrkeys = 1;
		}
	}
	else if (msg_p->opcode == OC_COLLS)
	{
		result = table_colls(tabela);
	}
	else if (msg_p->opcode == OC_UPDATE)
	{
		result = table_update(tabela, msg_p->content.entry->key, msg_p->content.entry->value);
	}
	else if (msg_p->opcode == OC_SIZE)
	{
		result = table_size(tabela);
	}

	msg_r = (struct message_t *)malloc(sizeof(struct message_t *));

	if (result == -1)
	{
		msg_r->opcode = OC_RT_ERROR;
		msg_r->c_type = CT_RESULT;
		msg_r->content.result = result;
		return msg_r;
	}

	/* Preparar mensagem de resposta */

	msg_r->opcode = msg_p->opcode + 1;
	if (msg_p->opcode == OC_PUT)
	{
		msg_r->c_type = CT_RESULT;
		msg_r->content.result = result;
	}
	else if (msg_p->opcode == OC_GET)
	{
		if (dataOrkeys == 0)
		{
			if (data == NULL)
			{
				struct data_t *not_found = (struct data_t *)malloc(sizeof(struct data_t));
				not_found->data = NULL;
				not_found->datasize = 0;
				msg_r->c_type = CT_VALUE;
				msg_r->content.data = not_found;
			}
			else
			{
				msg_r->c_type = CT_VALUE;
				msg_r->content.data = data_create2(data->datasize, data);
			}
		}
		else
		{
			msg_r->c_type = CT_KEYS;
			msg_r->content.keys = keys;
		}
	}
	else if (msg_p->opcode == OC_COLLS)
	{
		msg_r->c_type = CT_RESULT;
		msg_r->content.result = result;
	}
	else if (msg_p->opcode == OC_UPDATE)
	{
		msg_r->c_type = CT_RESULT;
		msg_r->content.result = result;
	}
	else if (msg_p->opcode == OC_SIZE)
	{
		msg_r->c_type = CT_RESULT;
		msg_r->content.result = result;
	}

	return msg_r;
}

/* Fun��o "inversa" da fun��o network_send_receive usada no table-client.
   Neste caso a fun��o implementa um ciclo receive/send:

	Recebe um pedido;
	Aplica o pedido na tabela;
	Envia a resposta.
*/
int network_receive_send(int sockfd)
{
	char *message_r, *message_p;
	//int msg_length;
	int message_size, msg_size, result;
	struct message_t *msg_p, *msg_r;

	/* Verificar par�metros de entrada */
	if (sockfd < 0)
	{
		return -1;
	}
	/* Com a fun��o read_all, receber num inteiro o tamanho da 
	   mensagem de pedido que ser� recebida de seguida.*/
	/* Verificar se a rece��o teve sucesso */
	if ((result = read_all(sockfd, (char *)&msg_size, _INT)) == 0)
	{
		perror("O cliente desligou-se");
		close(sockfd);
		return 0;
	}
	else if (result != _INT)
	{
		perror("Erro ao receber dados do cliente");
		close(sockfd);
		return -1;
	}

	/* Alocar mem�ria para receber o n�mero de bytes da
	   mensagem de pedido. */
	msg_size = ntohl(msg_size);
	msg_p = (struct message_t *)malloc(msg_size);
	message_p = (char *)malloc(msg_size);

	/* Com a fun��o read_all, receber a mensagem de resposta. */
	/* Verificar se a rece��o teve sucesso */
	if ((result = read_all(sockfd, message_p, msg_size)) == 0)
	{
		perror("O cliente desligou-se");
		close(sockfd);
		free_message(msg_p);
		free(message_p);
		return 0;
	}
	else if (result != msg_size)
	{
		perror("Erro ao receber dados do cliente");
		close(sockfd);
		free_message(msg_p);
		free(message_p);
		return -1;
	}

	/* Desserializar a mensagem do pedido */
	msg_p = buffer_to_message(message_p, msg_size);

	/* Verificar se a desserializa��o teve sucesso */
	if (msg_p == NULL)
	{
		free_message(msg_p);
		free(message_p);
		return -1;
	}
	/* Processar a mensagem */

	// Ver se é backup e se é um server secundario a receber
	int flagOC;
	if (msg_p->opcode == OC_PUT || msg_p->opcode == OC_UPDATE)
	{
		flagOC = 1;
	}

	if (type == SECONDARY && secondary->alive == ALIVE && (msg_p->opcode == OC_PUT || msg_p->opcode == OC_UPDATE))
	{
		int sizeServer = sizeof(struct sockaddr_in);
		struct sockaddr_in addr;

		if (getpeername(sockfd, (struct sockaddr *)&addr, &sizeServer) == -1)
		{
			//IP invalido
			free(msg_p);
			return -1;
		}
		char line[256];
		char *ip;
		char *str[80];

		configS = fopen("configS.ini", "a+");
		if (fgets(line, 80, configS) == NULL)
		{
			ip = inet_ntoa(addr.sin_addr);
			strcpy(str, ip);
			strcat(str, ":");
			int porta = atoi(port);
			porta--;
			char *newport[80];
			sprintf(newport, "%d", porta);
			strcat(str, newport);
			fprintf(configS, "%s\n", str);
		}
		fclose(configS);

		//Tudo pronto vamos la ligar ao primario
		secondary->primary = network_connect(str);

		//Ter a certeza que temos um estado replicado do servidor primario
		//update_state() //Update state nao esta a funcionar........

		//Process msg
		msg_r = invoke(msg_p);
	}
	else if (type == PRIMARY && (msg_p->opcode == OC_PUT || msg_p->opcode == OC_UPDATE))
	{
		if (primary->other_alive == ALIVE)
		{

			pthread_t comunicacao;
			struct thread_parameters tp;
			tp.msg_p = msg_p;
			if (primary->other_alive == ALIVE)
			{
				if (pthread_create(&comunicacao, NULL, &comm, (void *)&tp) != 0)
				{
					perror("Thread nao criada\n");
					exit(EXIT_FAILURE);
				}
			}
			//Esperar pelo resultado da thread
			int *resposta;
			pthread_join(comunicacao, (void **)&resposta);
			//Apos backup, fazer no primary
			msg_r = invoke(msg_p);

			//Se correu mal, marcar como down
			if (resposta != 0)
			{
				primary->other_alive = DEAD;
				close(primary->secondary->socket);
			}
			free(resposta);
		}
		else{
			//Apos backup, fazer no primary
			msg_r = invoke(msg_p);
		}
	}
	else if (type == PRIMARY && msg_p->opcode == OC_HEARTHBEAT)
	{
		msg_r = invoke(msg_p);
		primary->other_alive = ALIVE;
	}
	else
	{
		msg_r = invoke(msg_p);
	}

	/* Serializar a mensagem recebida */
	message_size = message_to_buffer(msg_r, &message_r);

	/* Verificar se a serializa��o teve sucesso */
	if (message_size <= 0) // Condicao hmmmm
	{
		free_message(msg_p);
		free_message(msg_r);
		free(message_p);
		return -1;
	}
	/* Enviar ao cliente o tamanho da mensagem que ser� enviada
	   logo de seguida
	*/
	/* Verificar se o envio teve sucesso */
	msg_size = htonl(message_size);
	if ((result = write_all(sockfd, (char *)&msg_size, _INT)) != _INT)
	{
		perror("Erro ao receber dados do cliente");
		close(sockfd);
		free_message(msg_p);
		free_message(msg_r);
		free(message_r);
		free(message_p);
		return -1;
	}

	/* Enviar a mensagem que foi previamente serializada */

	result = write_all(sockfd, message_r, message_size);

	/* Verificar se o envio teve sucesso */
	if (result != message_size)
	{
		perror("Erro ao receber dados do cliente");
		close(sockfd);
		free_message(msg_p);
		free_message(msg_r);
		free(message_p);
		free(message_r);
		return -1;
	}
	/* Libertar mem�ria */
	free_message(msg_p);
	free_message(msg_r);
	free(message_p);
	free(message_r);

	return 0;
}

/*
Trata das conecoes na socket pretendida
*/
int connectionHandler(int max_clients, int socket)
{
	struct sockaddr_in client;
	socklen_t size_client;
	struct pollfd polls[max_clients]; //Array para fazer o poll de possiveis conecoes
	int fds;						  //Numero de file descriptors
	int index, result, param = 1;

	//Setup das conecoes
	for (index = 0; index < max_clients; index++)
	{
		polls[index].fd = -1;
		polls[index].events = POLLIN;
	}
	//Primeira casa vai para a listening socket perceber quando ha eventos
	polls[0].fd = socket;
	polls[0].events = POLLIN;

	fds = 1;

	// Especifico para o tipo
	if (type == PRIMARY)
	{
		while ((result = poll(polls, max_clients, -1)) >= 0)
		{
			if (result > 0)
			{
				//Algum evento nos fds
				//Ver o tipo agora
				if ((polls[0].revents & POLLIN) && (fds < max_clients))
				{
					//Listening socket recebeu alguma coisa
					if ((polls[fds].fd = accept(polls[0].fd, (struct sockaddr *)&client, &size_client)) > 0)
					{
						polls[fds].events = POLLIN;
						fds++;
					}
					//Registrar o socket anterior no kernel como usado
					setsockopt(polls[fds - 1].fd, SOL_SOCKET, SO_REUSEADDR, (int *)&param, sizeof(param));
				}
				// Falta tratar do stdin

				// Conexao Secundario
				for (index = 1; index < fds; index++)
				{
					if (polls[index].revents & POLLIN)
					{
						char buffer[256];
						if (recv(polls[index].fd, buffer, sizeof(buffer), MSG_PEEK | MSG_DONTWAIT) == 0)
						{
							primary->other_alive = DEAD;
							close(polls[index].fd);
						}
						if (network_receive_send(polls[index].fd) != 0)
						{
							close(polls[index].fd);
							polls[index].fd = -1;
							fds--;
						}
					}
				}
			}
		}
		return 0;
	}
	else
	{
		while ((result = poll(polls, max_clients, -1)) >= 0)
		{
			if (result > 0)
			{
				//Algum evento nos fds
				//Ver o tipo agora
				if ((polls[0].revents & POLLIN) && (fds < max_clients))
				{
					//Listening socket recebeu alguma coisa
					if ((polls[fds].fd = accept(polls[0].fd, (struct sockaddr *)&client, &size_client)) > 0)
					{
						polls[fds].events = POLLIN;
						fds++;
					}
					//Registrar o socket anterior no kernel como usado
					setsockopt(polls[fds - 1].fd, SOL_SOCKET, SO_REUSEADDR, (int *)&param, sizeof(param));
				}
				// Falta tratar do stdin

				// Conexao Primario
				for (index = 1; index < fds; index++)
				{
					if (polls[index].revents & POLLIN)
					{
						char buffer[256];
						if (recv(polls[index].fd, buffer, sizeof(buffer), MSG_PEEK | MSG_DONTWAIT) == 0)
						{
							secondary->other_alive = -1;
							close(polls[index].fd);
						}
						if (network_receive_send(polls[index].fd) != 0)
						{
							close(polls[index].fd);
							polls[index].fd = -1;
							fds--;
						}
					}
				}
				if (type == PRIMARY)
				{
					return 0;
				}
			}
		}
	}
	return -1;
}

int main(int argc, char **argv)
{
	int listening_socket;
	struct sockaddr_in client;
	char line[256];

	//Fix das sockets nao serem fechadas normalmente
	signal(SIGPIPE, SIG_IGN);

	primary = malloc(sizeof(struct server_t));
	secondary = malloc(sizeof(struct server_t));

	//Setup do processo
	if (argc < 2)
	{
		printf("Servidor Primario: ./server <IP>:<port> <table1_size> [<table2_size> ...]\n");
		printf("Servidor Secundario: ./server <port>\n");
		printf("Exemplo de uso para primario: ./table-server 1337 127.0.0.1:1338 10 15 20 25\n");
		printf("Exemplo de uso para secundario: ./table-server 1338 \n");
		return -1;
	}
	else if (argc < 3)
	{
		port = argv[1];
		type = SECONDARY;
	}
	else
	{
		port = argv[1];
		type = PRIMARY;
		if (access("configP.ini", F_OK) == -1)
		{
			configP = fopen("configP.ini", "a+"); //criar ficheiro para dizer que os proximos primary nao sao originais
		}
		else
		{
			type = SECONDARY;
		}
	}
	if (type == PRIMARY)
	{
		int porta = atoi(argv[1]);
		listening_socket = make_server_socket(porta);
		if (listening_socket < 0)
		{
			perror("Falha a criar o servidor com a porta");
			printf("{%d}\n", porta);
			exit(EXIT_FAILURE);
		}

		/*********************************************************/
		/* Criar as tabelas de acordo com linha de comandos dada */
		/*********************************************************/
		table_skel_init(argv);

		//socklen_t size_client = sizeof(struct sockaddr_in);

		primary->addr = client;
		primary->alive = ALIVE;
		primary->socket = listening_socket;

		int give_up = 0, count = 0, max_tries = 5;
		do
		{
			primary->secondary = network_connect(argv[2]);
			if (primary->secondary == NULL)
			{
				printf("Impossivel conectar ao servidor secundario\nTem %d tentativas ate desistir e seguir em frente, tentativa #%d\n", max_tries, count);
				sleep(2);
				count++;

				if (count >= max_tries)
				{
					give_up = 1;
					primary->other_alive = DEAD;
					printf("Desisto\n");
				}
			}
			else
			{
				primary->other_alive = ALIVE;
				primary->secondary->alive = ALIVE;
				printf("Servidor Secundario encontrado\n");
				give_up = 1;
			}
		} while (give_up != 1);
	}
	else if (type == SECONDARY)
	{
		if (access("configS.ini", F_OK) == -1)
		{
			secondary->socket = make_server_socket(atoi(argv[1]));
			if (secondary->socket == NULL)
			{
				printf("Falha na socket do secundario\n");
				return -1;
			}
			secondary->alive = ALIVE;
			configS = fopen("configS.ini", "a+");
		}
		else
		{
			//Ligar ao ip do primario
			//Avisar que esta vivo com o hearthbeat e fazer update_state
			configS = fopen("configS.ini", "a+");
			fgets(line, sizeof(line), configS);
			secondary->socket = network_connect(line);
			if (secondary->primary == NULL)
			{
				perror("Impossivel conectar ao sevidor primario\n");
				exit(EXIT_FAILURE);
			}
			else
			{
				//Conexao foi um sucesso
				secondary->other_alive = ALIVE;
				if (hello(secondary->primary) != 0)
				{
					perror("Hearthbeat falhou\n");
					exit(EXIT_FAILURE);
				}
				if (update_state(secondary->primary) != 0)
				{
					perror("Impossivel fazer update\n");
					exit(EXIT_FAILURE);
				}
				updated = 1;
			}
			fclose(configS);
		}
	}
	else
	{
		//Nunca deve chegar aqui mas toma um debug
		printf("Type == %d\n", type);
	}

	//Logica de servidor, esperar por comunicacao
	while (1)
	{
		if (type == PRIMARY)
		{
			//Max clientes + stdin + listening socket do primary + secundario
			connectionHandler(NFDESC + 3, primary->socket);
		}
		else
		{
			connectionHandler(NFDESC, secondary->socket);
		}
	}

	free(primary);
	free(secondary);
	table_skel_destroy();
	fclose(configP);
	fclose(configS);
	remove("configS.ini");
	remove("configP.ini");
	return 0;
}