#include "dependencies.h"
#include "assist/message_utils.hpp"
#include "assist/queues.hpp"

#define SERVER_PORT 8060
#define CONFIG_FILE_PATH "./test/peerconfigtest.cfg"
#define DEFAULT_IP "127.0.0.1";
#define SERVER_BACKLOG 200
#define BUFSIZE 1024
#define DL_THREADS 5

//using namespace std;

//peer node global variables
std::string peer_id = "peer0";
std::string ip = "127.0.0.1";
int port = 8081;
std::string host_folder = "test/peer_files";
char super_peer_ip[15] = "127.0.0.1";
int super_peer_port = 8060;
int dl_attempts = 3;
size_t total_dl_size = 0;
double total_dl_time = 0;
namespace fs = std::filesystem;
typedef struct sockaddr SA;

peer_info my_info;//struct to store peer information

std::thread downloader_thread_pool[DL_THREADS];//downloader thread pool

std::vector<file_info> fileList;//vector to store files in the peer node
std::shared_mutex directoryMutex;//mutex to lock directory
std::mutex mtx;//general purpose mutex
std::mutex sconnect_mutex;//mutex to lock sconnect function
std::mutex file_host_map_mutex;//mutex to lock file_host_map
std::mutex dl_mutex;//mutex to lock dl_queue
std::condition_variable dl_cv; //condition variable for downloader thread

//map to store query messages and their response times
std::map<std::string, clock_t> query_map;
//map to store files and their corresponding hosts locations to download from
std::map<std::string, std::string> file_host_map;

FileQueue dl_queue;//queue to store files to be downloaded

void check(int n, const char *msg);
int sconnect(char IP[], int port);

/*** Load the config parameters from the config file ***/

bool loadConfig(const char filepath[]){//function to load configuration parameters from config file
	char real_path[PATH_MAX+1];
	realpath(filepath,real_path);
	std::ifstream in(real_path);
	if(!in.is_open()){
		std::cout << "config file could not be opened\n";
        in.close();
		return false;
	}
	std::string param;
	std::string value;

	while(!in.eof()){
		in >> param;
		in >> value;
        if(param == "PEER_ID"){
            peer_id = value;
            my_info.peer_id = peer_id;
        }
        else if(param == "IP"){
            ip = value;
            strcpy(my_info.ip,ip.c_str());
        }
		else if(param == "PORT"){
			port = stoi(value);
            my_info.port = port;
		}
        else if(param == "HOST_FOLDER"){
            host_folder = value;
        }
        else if(param == "SUPER_PEER_ADDR"){
            strcpy(super_peer_ip,value.c_str());
        }
        else if(param == "SUPER_PEER_PORT"){
            super_peer_port = stoi(value);
        }
        else if(param == "DOWNLOAD_ATTEMPTS"){
            dl_attempts = stoi(value);
        }
	}
	in.close();
	return true;
}

/*** Helper functions for peer ***/

void check(int n, const char *msg, bool fatal){//function to check for errors, print error message and exit if fatal
    std::string err = peer_id + ":" + msg;
	if(n<0){
		perror(err.c_str());
	}
    if(n < 0 && fatal){
        perror(err.c_str());
        exit(1);
    }
}

void checkUploadDirect(){//check if the upload directory exists, if not, create it
    struct stat st;
    char* d = new char[host_folder.length()];
    strcpy(d, host_folder.c_str());
    if(((stat(d,&st)) == 0) && (((st.st_mode) & S_IFMT) == S_IFDIR)){
        //directory exists
    }else{
        if(mkdir(d, S_IRWXU | S_IRWXG | S_IROTH)){
            printf("Failed to create directory %s\n",d);
            exit(EXIT_FAILURE);
        }
    }
}

void checkDownloadDirect(){//check if the download directory exists, if not, create it
    struct stat st;
    char* d = new char[host_folder.length()];
    strcpy(d, host_folder.c_str());
    if(((stat(d,&st)) == 0) && (((st.st_mode) & S_IFMT) == S_IFDIR)){
        //directory exists
    }else{
        if(mkdir(d, S_IRWXU | S_IRWXG | S_IROTH)){
            printf("Failed to create directory %s\n",d);
            exit(EXIT_FAILURE);
        }
    }
}

void updateFileList(){//update the list of files in the upload directory
    std::string path = get_current_dir_name();
    path += "/" + host_folder;
    fileList.clear();

    std::shared_lock<std::shared_mutex> lock(directoryMutex);

    for(const auto & entry : fs::directory_iterator(path)){
        //std::cout << entry.path() << std::endl;
        file_info f;
        f.file_name = entry.path().filename();
        f.file_size = fs::file_size(entry.path());
        fileList.push_back(f);
    }
    mtx.lock();
    my_info.files = fileList;
    mtx.unlock();
}

std::vector<file_info> getFileList(){//return the list of files in the upload directory
    updateFileList();
    return fileList;
}

std::string generateQueryName() {//generate a unique query name for each query
    std::uniform_int_distribution<> dis(0, 15);
    std::stringstream ss;
    std::random_device rd;
    std::mt19937 gen(rd());
    ss << std::hex;
    for (int i = 0; i < 5; ++i) {
        ss << dis(gen);
    }
    ss << "-";
    for (int i = 0; i < 5; ++i) {
        ss << dis(gen);
    }
    ss << "-";
    for (int i = 0; i < 5; ++i) {
        ss << dis(gen);
    }
    ss << "-";
    ss << dis(gen) % 4 + 8;
    for (int i = 0; i < 5; ++i) {
        ss << dis(gen);
    }
    ss << "-";
    for (int i = 0; i < 12; ++i) {
        ss << dis(gen);
    }
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    std::stringstream name_ss;
    name_ss << ss.str() << "_" << std::setfill('0') << std::setw(20) << time;

    return name_ss.str();
}

std::vector<std::string> splitString(const std::string& s, char delimiter)                                                                                                                          
{                                                                                                                                                                                             
   std::vector<std::string> re;                                                                                                                                                           
   std::string st;                                                                                                                                                                         
   std::istringstream ss(s);                                                                                                                                                                  
   while (std::getline(ss, st, delimiter))                                                                                                                                                 
   {                                                                                                                                                                                          
      re.push_back(st);                                                                                                                                                                
   }                                                                                                                                                                                          
   return re;                                                                                                                                                                             
}

/*** Socket functions for network communications ***/

int slisten(int port){//function to create a socket and listen for incoming connections on the specified port
    int opt = 1;

	int server_socket;
	sockaddr_in server_addr;

	check((server_socket = socket(AF_INET, SOCK_STREAM, 0)),"failed to create socket for listening",true);//create socket

	check(setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)),"failed to set socket options",true);

	server_addr.sin_family = AF_INET;//configure server
	server_addr.sin_addr.s_addr = INADDR_ANY;
	server_addr.sin_port = htons(port);

	check(bind(server_socket,(sockaddr*)&server_addr,sizeof(server_addr)),"failed to bind peer socket",true);//bind socket
	check(listen(server_socket, SERVER_BACKLOG),"peer failed to listen to connections",true);//listen for connections

	//printf("Waiting for connections on port %d...\n",port);
    return server_socket;
}

int sconnect(char IP[], int port){//connect to super-peer or another peer node and return the socket
    std::lock_guard<std::mutex> lock(sconnect_mutex);
    
    int sock = 0, client_fd;
    struct sockaddr_in server_addr;
    if((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0){
        printf("error creating socket connection with %s:%d\n",IP,port);
        return -1;
    }
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if(inet_pton(AF_INET, IP, &server_addr.sin_addr) <=0){
        printf("\nInvalid address or address not supported\n");
        return -1;
    }
    if((client_fd = connect(sock, (SA*)&server_addr, sizeof(server_addr))) < 0){
        printf("Connection to server on port %d failed!\n",port);
        return -1;
    }
    //printf("Connection to server on port %d successful\n",port);
    return sock;
}

/**********************************************************************************************/

/*** Core functions for the peer's operation ***/

int downloadFile(int socket, char filename[]){//download a file specified by it's name from a peer using the specified socket
    
    int valread;
    char read_buffer[BUFSIZE] ={0};
    int sock = socket;
    MessageHeader h;
    h.length = strlen("download");
    send(sock, &h, sizeof(h), 0); //send message header
    if((send(sock, "download", strlen("download"), 0))<0){ //send download command to server
        printf("error sending download command to server\n");
        close(sock);
        return -1;
    }

    h.length = strlen(filename);
    send(sock, &h, sizeof(h), 0); //send message header
    if((send(sock, filename, strlen(filename), 0))<0){ //send filename to server
        printf("error sending file name to server\n");
        close(sock);
        return -1;
    }
    
    MessageHeader header;
    //receive message header
    valread = recv(sock, &header, sizeof(header),0);
    //receive message data
    valread = read(sock, read_buffer, header.length);
    if(valread < 0){
        printf("error reading response from server"); 
        close(sock);
        return -1;
    }
    //if file not found, tell user
    if(strncmp(read_buffer, "File not found", strlen("File not found")) == 0){
        printf("File not found: %s\n", filename);
        close(sock);
        return -1;
    }

    printf("Downloading file...\n");
    send(sock, "r", 1, 0);//signal to server we are ready to download

    //filename[strlen(filename)-1] = '\0';

    //create path to download folder
    std::string n_host_folder = host_folder + "/" + filename;
    //lock directory

    FILE *fp = fopen(n_host_folder.c_str(), "w");
    //To time the speed of transfer
    clock_t start;
	double dur;
	start = clock();
	size_t bytes_recvd = 0;

    while((valread = read(sock, read_buffer, BUFSIZE))>0){
        //printf("%s",read_buffer);
        fwrite(read_buffer, valread,1,fp);
        bytes_recvd+=valread;
    }
    //record speed and size of transfer
    dur = (clock()-start)/((double)CLOCKS_PER_SEC);
    printf("Received %zu bytes in %.16f seconds\n",bytes_recvd, dur);
    total_dl_size +=bytes_recvd;
    total_dl_time +=dur;
    fclose(fp);
    if(valread < 0){
        printf("error reading response from server\n\n"); 
        close(sock);
        return -1;
    }
    file_info newFile = {filename, (int)bytes_recvd};
    mtx.lock();
    my_info.files.push_back(newFile);
    mtx.unlock();
    close(sock);
    return 0;
}

//download a file from a peer given a list of peers that have the file, and the file name
void downloadFromPeer(std::string peer_ip, int peer_port, char* file_name){
    printf("Connecting to peer %s:%d\n", peer_ip.c_str(), peer_port);
    //connect to the peer
    int peer_sock = sconnect((char*)peer_ip.c_str(), peer_port);
    //download the file
    int dls = downloadFile(peer_sock, file_name);
    close(peer_sock);
    if(dls == -1){
        printf("Error downloading file from peer %s:%d\n", peer_ip.c_str(), peer_port);
    }
    else{
        printf("File downloaded successfully from peer %s:%d\n", peer_ip.c_str(), peer_port);
    }    

}

//query the super peer that acts as an indexing server for the specified file name
int queryFile(char* file_name){
    int sock = sconnect(super_peer_ip, super_peer_port);
    if(sock < 0){
        printf("error connecting to super peer\n");
        return -1;
    }
    //create QueryMessage
    QueryMessage qm;
    qm.message_id = generateQueryName();
    qm.ttl = 5;
    qm.file_name = file_name;
    qm.origin_peer_id = peer_id;
    qm.origin_ip_address = ip;
    qm.origin_port = port;

    query_map.insert(std::pair<std::string, clock_t>(qm.message_id, clock()));

    std::vector <std::string> valid_peers;
    MessageHeader h;
    h.length = strlen("Query");
    
    check(send(sock, &h, sizeof(h), 0), "error sending message header",false); //send message header
    check(send(sock, "Query", strlen("Query"), 0), "error sending find request to super peer",false);
    char rb[10] = {0};
    check(recv(sock, rb, sizeof(rb), 0), "error receiving OK to send Query Message",false);
    if(strncmp(rb, "OK", strlen("OK")) != 0){
        printf("error receiving OK to send Query Message\n");
        close(sock);
        return -1;
    }

    std::vector<uint8_t> msg = serializeQueryMessage(qm);
    uint32_t msg_size = msg.size();
    check(send(sock, &msg_size, sizeof(msg_size), 0), "error sending query message to super peer",false);
    check(send(sock, msg.data(), msg.size(), 0), "error sending query message to super peer",false);
    check(recv(sock, rb, 7, 0), "error receiving query message response",false);
    if(strcmp(rb,"SUCCESS") != 0){
        printf("Query Message not sent successfully\n");
        close(sock);
        return -1;
    }
    close(sock);
    return 0;
}


int update_hosted_files_all(){
    //connect to the server
    int sock;
    if((sock= sconnect(super_peer_ip, super_peer_port))<0){
        printf("error connecting to super peer");
        return -1;
    }
    updateFileList();
    char buffer[BUFSIZE] = {0};
    mtx.lock();
    serialize_peer_info(my_info, buffer);
    mtx.unlock();
    MessageHeader h;
    h.length = strlen("update");    
    //send the command
    if(((send(sock, &h, sizeof(h), 0)) < 0) || ((send(sock, "update", strlen("update"), 0)) < 0)){
        printf("error sending update command to super peer\n");
        close(sock);
        return -1;
    }
    //process the response
    char rb[BUFSIZE] = {0};
    if((recv(sock, rb, BUFSIZE, 0)) < 0){
        printf("error receiving OK to send peer info for update\n");
        close(sock);
        return -1;
    }
    if(strcmp(rb, "OK") != 0){
        printf("Failed to update with super peer\n");
        close(sock);
        return -1;
    }
    //send the peer info
    if(send(sock, buffer, BUFSIZE, 0) < 0){
        printf("error sending peer info to super peer for update\n");
        close(sock);
        return -1;
    }
    close(sock);
    return 0;
}

/*
//update the server with a single file that this peer is hosting
int add_hosted_file(char filename[], int file_size){
    int sock = sconnect(super_peer_ip, super_peer_port);
    if(sock < 0){
        printf("error connecting to super peer");
        return -1;
    }
    //send the command
    MessageHeader h;
    h.length = strlen("single_add");
    if(((send(sock, &h, sizeof(h), 0)) < 0) || ((send(sock, "single_add", strlen("single_add"), 0)) < 0)){
        printf("error sending single_add command to super peer\n");
        close(sock);
        return -1;
    }
    //process the response
    char rb[BUFSIZE] = {0};
    if((recv(sock, rb, BUFSIZE, 0)) < 0){
        printf("error receiving OK to send peer info for single_add\n");
        close(sock);
        return -1;
    }
    if(strcmp(rb, "OK") != 0){
        printf("Failed to update with super peer\n");
        close(sock);
        return -1;
    }
    //send peer_id
    h.length = peer_id.length();
    if((send(sock,&h,sizeof(h),0)<0) || (send(sock, &peer_id, sizeof(peer_id), 0) < 0)){
        printf("error sending peer_id to super peer for single_add\n");
        close(sock);
        return -1;
    }
    //send filename
    h.length = strlen(filename);
    if((send(sock,&h,sizeof(h),0)<0) || (send(sock, filename, strlen(filename), 0) < 0)){
        printf("error sending filename to super peer for single_add\n");
        close(sock);
        return -1;
    }
    send(sock, (void*)file_size, sizeof(file_size), 0);

    return 0;
}
*/


//register the peer with the server, and transmit the files that are currently hosted
//returns 0 on success, -1 on failure
int register_peer(){
    //connect to the server
    int sock;
    if((sock= sconnect(super_peer_ip, super_peer_port))<0){
        printf("error connecting to super peer");
        return -1;
    }
    updateFileList();
    char buffer[BUFSIZE] = {0};
    serialize_peer_info(my_info, buffer);
    MessageHeader h;
    h.length = strlen("register");    
    //send the command
    if(((send(sock, &h, sizeof(h), 0)) < 0) || ((send(sock, "register", strlen("register"), 0)) < 0)){
        printf("error sending register command to super peer\n");
        close(sock);
        return -1;
    }
    //process the response
    char rb[BUFSIZE] = {0};
    if((recv(sock, rb, BUFSIZE, 0)) < 0){
        printf("error receiving OK to send peer info\n");
        close(sock);
        return -1;
    }
    if(strcmp(rb, "OK") != 0){
        printf("Failed to register with super peer\n");
        close(sock);
        return -1;
    }
    //send the peer info
    if(send(sock, buffer, BUFSIZE, 0) < 0){
        printf("error sending peer info to super peer\n");
        close(sock);
        return -1;
    }
    close(sock);
    return 0;
}

//unregister the peer with the server, this is called when the peer is shutting down
//returns 0 on success, -1 on failure
int unregister_peer(){
    //connect to the server
    int sock;
    if((sock= sconnect(super_peer_ip, super_peer_port))<0){
        printf("error connecting to super peer for unregistration\n");
        return -1;
    }
    MessageHeader h;
    h.length = strlen("unregister");
    //send the unregister command
    if(
        (send(sock, &h, sizeof(h), 0)) < 0 || (send(sock, "unregister", strlen("unregister"),0)) < 0
    ){
        printf("error sending unregister command to super peer\n");
        close(sock);
        return -1;
    }
    //process the response
    char rb[BUFSIZE] = {0};
    if((recv(sock, rb, BUFSIZE, 0))<0){
        printf("error receiving unregister response from super peer\n");
        close(sock);
        return -1;
    }
    if(strcmp(rb, "OK") == 0){
        printf("Successfully unregistered with super peer\n");
    }
    else{
        printf("Failed to unregister with super peer\n");
    }
    //send the peer info
    char buffer[BUFSIZE] = {0};
    mtx.lock();
    serialize_peer_info(my_info, buffer);
    mtx.unlock();

    if((send(sock, buffer, BUFSIZE, 0))<0){
        printf("error sending peer info to super peer\n");
        close(sock);
        return -1;
    }

    close(sock);
    return 0;
}

//handle QueryHit message
void handleQueryHit(int client_socket){
    //printf("Received QueryHit\n");
    send(client_socket, "OK", strlen("OK"), 0);
    QueryHitMessage qhm;
    uint32_t msg_len;
    if(recv(client_socket, &msg_len, sizeof(msg_len), 0) < 0){
        printf("Error receiving query message length\n");
        close(client_socket);
        return;
    }
    std::vector<uint8_t> serialized_data(msg_len);
    size_t total_size = 0;
    while(total_size < msg_len){
        ssize_t bytes_read = recv(client_socket, serialized_data.data() + total_size, msg_len - total_size, 0);
        if(bytes_read < 0){
            printf("Error receiving query message content\n");
            close(client_socket);
            return;
        }
        total_size += bytes_read;
    }
    if(total_size == 0){
        printf("Error receiving query message content\n");
        close(client_socket);
        return;
    }
    std::vector<uint8_t> serialized_vector(serialized_data.begin(),serialized_data.end());
    try{
        qhm = deserializeQueryHitMessage(serialized_vector);
    }catch(const std::exception& e){
        printf("Error deserializing query hit message content\n");
        return;
    }
    send(client_socket, "SUCCESS", strlen("SUCCESS"), 0);
    close(client_socket);
    //if this is the first time we have received a QueryHit for this file, print the response time
    //and store the host location for the file in the file_host_map
    double dur = (clock()-query_map[qhm.message_id])/((double)CLOCKS_PER_SEC);
    printf("Received QueryHit for file: %s at: %s:%d, response time: %.16f\n",qhm.file_name.c_str(), qhm.ip_address.c_str(), qhm.port, dur);
    std::lock_guard<std::mutex> lock(file_host_map_mutex);
    if(file_host_map.find(qhm.file_name) == file_host_map.end()){
        //store the host location for the file in the file_host_map
        std::string host_loc = qhm.ip_address + ":" + std::to_string(qhm.port);
        file_host_map[qhm.file_name] = host_loc;
        //add the file to the download queue
        dl_queue.enqueue(qhm.file_name, host_loc);
        //notify the downloader threads we have a file to download
        dl_cv.notify_one();
        fflush(stdout);
    }
    //if this is not the first time we have received a QueryHit for this file, do nothing (ignore it)
    return;
}

void handleDownloadRequest(int client_socket){
    char buffer[BUFSIZE] = {0};
    MessageHeader h;
    recv(client_socket, &h, sizeof(h), 0);
    recv(client_socket, buffer, h.length, 0);
    printf("Received download request for file name: %s\n", buffer);
    std::string file_name = buffer;
    std::string path = get_current_dir_name();
    path += "/" + host_folder + "/" + file_name;
    FILE *fp = fopen(path.c_str(), "rb");
    if(fp == NULL){
        printf("File not found\n");
        h.length = strlen("File not found");
        send(client_socket, &h, sizeof(h), 0);
        send(client_socket, "File not found", h.length, 0);
        close(client_socket);
        //if the file is not found, update the server and tell it to remove the file from the list of hosted files
        //update_hosted_files(sconnect(super_peer_ip, super_peer_port), 1, (char*)file_name.c_str());
        return;
    }else{
        //printf("File found\n");
        h.length = strlen("File found");
        send(client_socket, &h, sizeof(h), 0);
        send(client_socket, "File found", h.length, 0);
    }

    read(client_socket, buffer, 1);

    std::clock_t start;
    double dur;
    start = std::clock();
    size_t bytes_sent = 0;
    size_t bytes_read;

    while((bytes_read = fread(buffer, 1, BUFSIZE, fp)) > 0){    
        bytes_sent += bytes_read;
        send(client_socket, buffer, bytes_read, 0);
    }
    dur = (std::clock() - start) / (double) CLOCKS_PER_SEC;
    printf("Sent %zu bytes in %.16f seconds\n", bytes_sent, dur);
    
    close(client_socket);
}

void downloader_thread_function(){
    while(1){
        std::unique_lock<std::mutex> lock(dl_mutex);
        if(dl_queue.empty()){
            dl_cv.wait(lock);
        }
        std::pair<std::string, std::string> file_to_dl = dl_queue.dequeue();
        std::string file_name = file_to_dl.first;
        std::string host_loc = file_to_dl.second;
        bool already_downloaded = false;
        //make sure we don't already have the file
        for(auto it = my_info.files.begin(); it != my_info.files.end(); it++){
            if(it->file_name == file_name){
                already_downloaded = true;
            }
        }
        if(!already_downloaded){
            std::string ip_address = host_loc.substr(0, host_loc.find(":"));
            int port = std::stoi(host_loc.substr(host_loc.find(":")+1));
            downloadFromPeer(ip_address, port, (char*)file_name.c_str());
        
        }
    }
}

/**************************************************************************************************/

/*** Functions for peer to act as a server ***/

void handleConnection(int client_socket){
    char buffer[BUFSIZE] = {0};
    MessageHeader h;
    //receive the command
    recv(client_socket, &h, sizeof(h), 0);
    recv(client_socket, buffer, h.length, 0);
    //printf("Received message: %s\n", buffer);
    if(strcmp(buffer, "QueryHit")==0){
        handleQueryHit(client_socket);
    }
    else if(strcmp(buffer, "download") == 0){
        handleDownloadRequest(client_socket);
    }else{
        close(client_socket);
    }
}

void listenForConnections(){
    int sock = slisten(port);
    struct sockaddr_in client_addr;
    int addr_size;
    check(sock, "error listening for connections",true);
    while(1){
        int client_sock = accept(sock, (sockaddr*)&client_addr, (socklen_t*)&addr_size);
        check(client_sock, "error accepting connection",true);
        printf("\nAccepted connection from %s:%d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
        std::thread t1(&handleConnection, client_sock);
        t1.detach();
    }
}
/*************************************************************************************************************/

/*** Functions for interactivity or automation ***/

//if there was a list of files to download passed as an argument, download them
void loadDownloadList(std::vector<std::string> files){
    for(auto file_name : files){
        std::ifstream file(file_name);
        std::string str; 
        while (std::getline(file, str))
        {
            queryFile((char*)str.c_str());
            sleep(2);
        }
        update_hosted_files_all();
        sleep(5);
    }
    return; 
}

void interactiveMode(){
    char buffer[BUFSIZE];
    while(1){
        printf("Enter file name to download: ");
        scanf("%s", buffer);
        if(strcmp(buffer, "exit") == 0){
            exit(0);
        }else{
            queryFile(buffer);
            
        }
    }
}

/*** Signal handlers ***/

//handle sigpipe
void handle_sigpipe(int sig) {
    void *array[10];
    size_t size;

    // get void*'s for all entries on the stack
    size = backtrace(array, 10);

    // print out all the frames to stderr
    fprintf(stderr, "Error: signal %d:\n", sig);
    backtrace_symbols_fd(array, size, STDOUT_FILENO);
    //exit(1);
}
//handle ctrl+c
void interruptHandler(int sig){
    printf("SIGNAL: %d Exiting...\n",sig);
    exit(0);
}
//unregister peer from the server when the program exits, try 3 times if it fails
void exitRoutine(){
    int n = 0;
    while(n < 3 && unregister_peer() != 0){
        sleep(1);
        n++;
    }
    if(n == 3){
        printf("Failed to unregister peer from server\n");
        exit(1);
    }
    exit(0);
}

/***********************************************************************************************************/

int main(int argc, char *argv[]){
    bool interactive = false;
    bool hostonly = false;
    if(argc < 2){
        printf("Usage: <config file path> <files_to_download.txt> <-i for interactive mode> <-h for host only mode>\n");
        //loadConfig(CONFIG_FILE_PATH);
    }else{
        loadConfig(argv[1]);
    }

    for(int i = 0; i < argc; i++){
        if(strcmp(argv[i], "-i") == 0){
            interactive = true;
        }
        if(strcmp(argv[i], "-h") == 0){
            hostonly = true;
        }
    }
    //ensure that the folder for the hosted files exists
    checkDownloadDirect();
    checkUploadDirect();
    //set up signal handlers and exit routine
    atexit(exitRoutine);
    signal(SIGPIPE, handle_sigpipe);
    signal(SIGINT, interruptHandler);
    //register peer with the server, try 3 times if it fails
    int n = 0;
    while (n < 3 && register_peer() != 0){
        n++;
        sleep(1);
    }
    if(n == 3){
        printf("Failed to register peer with server\n");
        exit(1);
    }

    for(int i = 0; i < DL_THREADS; i++){
        std::thread t1(&downloader_thread_function);
        t1.detach();
    }

    std::thread t1(listenForConnections);
    //if the -h flag was passed, only host files and listen for connections
    if(hostonly){
        printf("Peer is only hosting files\n");
        while(1){
            t1.join();
        }
        return 0;
    }
    //start the thread to listen for connections
    t1.detach();
    //start the interactive mode thread if the -i flag was passed
    if(interactive){
        std::thread it(interactiveMode);
        it.join();
    }
    //if there was a list of files to download passed as an argument, download them
    else{
        sleep(2);
        std::string arg2 = argv[2];
        std::vector<std::string> dl_list = splitString(arg2, ',');
        
        loadDownloadList(dl_list);
        while(!query_map.empty()){
            sleep(1);
        }
        printf("All files downloaded\n");
    }
    printf("Going to host only mode\n");
    fflush(stdout);
    while(1){
        sleep(15);
    }
    return 0;
}