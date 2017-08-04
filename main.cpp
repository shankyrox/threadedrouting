#include <iostream>
#include <thread>
#include <map>
#include <vector>
#include <atomic>
#include <queue>
#include <mutex>
#include <fstream>

using namespace std;

enum Priority{
    LOW = 0,
    MEDIUM,
    HIGH
};

//Row = Data Center capacity, Column = Message priority
int dcProccessTime[3][3] = { {2, 4, 5}, {1, 2, 4}, {1, 1, 2} };

class Msg{
public:
    char src;
    char dstn;
    Priority pri;
    time_t intime;
    time_t outtime;
    vector<char> path;  // List to track path followed by each msg
    /* Parameterised constructor for creating new Msg instances.
     * inttime & outtime are set to current system time. Outtime will be updated when msg reaches the destination.
     */
    Msg(char src, char dstn, Priority pri){
        this->src = src;
        this->dstn = dstn;
        this->pri = pri;
        time(&intime);
        time(&outtime);
    }
    Msg(){}
};

map<char, thread> T; // List of threads corresponding to each Data center which is up & running
thread routingTableThread; // thread object which is responsible for iteratively computing routing table
ofstream debug("debug.txt"); // debug file object
ofstream output("output.txt"); // outfile file object
atomic_bool ready(false); // flag to ensure initial nextHop matrix built before bringing data center up
atomic_bool Exit(false);  // flag to make the threads exit when user asks

/* Name : Compare
 *
 * Description :
 *      Utility class to be used with priority queue. Any message with higher priority than other messages will be
 *      dequeued first. In case message priority is same, message with lesser time is placed first.
 */
class Compare{
public:
    bool operator()(const pair<int, Msg> m1, const pair<int, Msg>m2){
        if(m1.second.pri != m2.second.pri)
            return m1.second.pri < m2.second.pri;
        return m1.first > m2.first;
    }
};


/* Name : Compare2
 *
 * Description :
 *      Utility class to be used with priority queue. Any message with higher priority than other messages will be
 *      dequeued first.
 */
class Compare2{
public:
    bool operator()(const pair<time_t, Msg> p1, const pair<time_t, Msg> p2){
        return p1.first > p2.first;
    }
};

/* Name : Node
 *
 * Description :
 *      A class representing a data center node.
 */
class Node{
private:
    // locks used for making queues thread safe
    mutex currMsgLock;
    mutex arrivingMsgLock;
    /* Queue of messages which are being currently processed at this node. Priority queue ensures that higher priority
     * messages are processed first.
     * int (first)   = remaining processing time for this Msg
     * Msg (second)  = Msg instance at this node
     */
    priority_queue<pair<int, Msg>, vector<pair<int,Msg>>, Compare> currMsg;
    /* Queue of messages which have been forwarded by a neighboring node towards this Node. They will arrive with some
     * delay due to edge weight (msg takes a predefined time over the wire)
     */
    priority_queue<pair<time_t, Msg>, vector<pair<time_t, Msg>>, Compare2> arrivingMsg;
public:
    Priority pri;                // Capacity of this data center
    char key;                    // key of this data center
    map<char, int> neighbors;    // List of neighbors, char = neighbor/dstn key; int = edge weight
    atomic_bool terminate;       // flag to terminate thread when user asks
    // Parameterised constructor
    Node(char key){
        this->key = key;
        terminate = false;
    }
    //Default constructor
    Node(){
        terminate = false;
    }
    void pushDataCurrMsg(Msg msg);
    void pushDataArrivingMsg(Msg msg, time_t t);
    void processArrivedMsges();
    void processCurrMsg();
    int getCurrentDelay(Priority pri_msg);
    void deleteAllQueues();
};

class Graph{
private:
    mutex nextHopLock;      // lock for nextHop matrix
    // nextHop matrix stores next hop key for a given msg priority, src, & dstn
    map<Priority , map<char, map<char, char>>> nextHop;
public:
    map<char, Node*> NodeList;  // List of all the Nodes present in our graph/Network
    mutex NodeListLock;         // Mutex for locking graph
    char readNextHop(Priority p, char src, char dstn);
    void setNextHopMatrix(map<Priority , map<char, map<char, char>>>& nextHopLocal);
};
Graph graph;        // Only 1 Graph instance

// This namespace contains several utility functions
namespace Utils{
    void createNewNode(char key);
    Priority convertCharToPriority(char x);
    int getEdgeWt(map<char, int>::iterator it, Priority pri_msg);
    bool isNextHopValid(char ch);
    void displayNextHopData(map<Priority , map<char, map<char, char>>> nextHopLocal);
    void displayMsg(Msg msg);
    void displayGraphData();
    void Dijkstra(char src, Priority pri_msg, map<char, char>& parentNode);
    char getParent(char src, char dstn, map<char, char> &parentNode);
}





/***********************      NODE CLASS FUNCTIONS      ********************************/
/* Name : pushDataCurrMsg
 *
 * Arguments :
 *      msg     -> A message instance which has to be pushed to currMsg queue of Node
 *
 * Description :
 *      Add a Msg to the queue of messages being processed at this node. Acquire currMsgLock before pushing to avoid
 *      race conditions
 *
 * Return : None
 */
void Node::pushDataCurrMsg(Msg msg){
    Priority pri_msg = msg.pri;
    int processTime = dcProccessTime[pri][pri_msg];
    currMsgLock.lock();
    currMsg.push(make_pair(processTime, msg));
    currMsgLock.unlock();
}


/* Name : pushDataArrivingMsg
 *
 * Arguments :
 *      msg     -> msg instance which is sent by the neighboring node and will arrive at a future time
 *      t       -> time at which msg will reach this Node. t is obtained by adding edge wt to current system time
 *
 * Description :
 *      Add msg to queue of messages which will arrive at this Node at a future time. Acquire arrivingMsgLock before
 *      pushing to avoid race conditions. Time at which message will arrive is determined by adding edge weight to
 *      current system time
 *
 * Return : None
 */
void Node::pushDataArrivingMsg(Msg msg, time_t t){
    arrivingMsgLock.lock();
    arrivingMsg.push(make_pair(t, msg));
    arrivingMsgLock.unlock();
}

/* Name : processArrivedMsges
 *
 * Arguments : None
 *
 * Description :
 *      Iterate over the arrivingMsg queue. If arrival time has crossed the current system time, this means that msg
 *      has arrived at this Node. Push this msg for processing to currMsg queue. Acquire necessary locks to avoid race
 *      conditions
 *
 * Return : None
 */
void Node::processArrivedMsges(){
    arrivingMsgLock.lock();
    time_t now;
    time(&now);
    while(!arrivingMsg.empty() && arrivingMsg.top().first<=now){
        Msg msg = arrivingMsg.top().second;
        pushDataCurrMsg(msg);
        currMsgLock.unlock();
        arrivingMsg.pop();
    }
    arrivingMsgLock.unlock();
}

/* Name : processCurrMsg
 *
 * Arguments : None
 *
 * Description :
 *      Process the message at the top of currMsg queue.
 *       - If the message has still processing time remaining, decrement time remaining and push back to queue
 *       - If the message has finished processing and current node is the dstn for this msg, the display it
 *       - Else find next hop from nextHop matrix and push to neighboring node's arrivingMsg queue
 */
void Node::processCurrMsg(){
    currMsgLock.lock();
    if(currMsg.empty()){
        currMsgLock.unlock();
        return;
    }
    if(currMsg.top().first==1) {
        Msg msg = currMsg.top().second;
        currMsg.pop();
        currMsgLock.unlock();
        msg.path.push_back(key);
        if(msg.dstn==key){
            time(&msg.outtime);
            msg.outtime = msg.outtime+1;
            Utils::displayMsg(msg);
        } else {
            char nextHopKey = graph.readNextHop(msg.pri, key, msg.dstn);
            if(!Utils::isNextHopValid(nextHopKey)) {
                output << "NO PATH EXISTS FOR :\n";
                Utils::displayMsg(msg);
                output << "kEY = " << key << endl;
                return;
            }
            time_t now;
            time(&now);
            if(graph.NodeList[key]->neighbors.find(nextHopKey)!=graph.NodeList[key]->neighbors.end()){
                int wt = graph.NodeList[key]->neighbors[nextHopKey]; // make it thread safe - add a try catch block here
                now = now+wt+1;
                graph.NodeList[nextHopKey]->pushDataArrivingMsg(msg, now);
            } else {
                output << "NEIGHBOUR DELETED : " << nextHopKey << endl;
            }

        }
    } else {
        int rem = currMsg.top().first-1;
        Msg msg = currMsg.top().second;
        currMsg.pop();
        currMsg.push(make_pair(rem, msg));
        currMsgLock.unlock();
    }
}

/* Name : getCurrentDelay
 *
 * Arguments :
 *      pri_msg     -> priority of message based on which delay incurred is computed
 *
 * Description :
 *      This function is invoked while computing shortest path between 2 Nodes. It specifies the delay which a message
 *      of given priority will have based on queue of messages(currMsg) which are pending for processing at this Node
 *
 * Returns :
 *      Delay in seconds, which a message would priority - pri_msg incur at current time
 */
int Node::getCurrentDelay(Priority pri_msg){
    try {
        int delay = 0;
        currMsgLock.lock();
        vector<pair<int, Msg>> tmp;
        while (!currMsg.empty() && currMsg.top().second.pri >= pri_msg) {
            delay += currMsg.top().first;
            tmp.push_back(currMsg.top());
            currMsg.pop();
        }
        while (!tmp.empty()) {
            currMsg.push(tmp[0]);
            tmp.erase(tmp.begin());
        }
        currMsgLock.unlock();
        return delay;
    } catch (...){
        currMsgLock.unlock();
        cout << "ERROR in computing currentDelay for key=" << key << endl;
        return 0;
    }
}

/* Name : deleteAllQueues
 *
 * Arguments : None
 *
 * Description :
 *      It is a utility function used to clear up currMsg queue and arrivingMsg queue when this node is going down.
 *      Messages for which src or dstn are not this node, are recovered by re-pumping them in currMsg queue of src node
 *
 * Returns : None
 */
void Node::deleteAllQueues() {
    for(map<char,int>::iterator it=neighbors.begin(); it!=neighbors.end(); ++it){
        char src = it->first;
        graph.NodeList[src]->neighbors.erase(key);
    }
    currMsgLock.lock();
    while(!currMsg.empty()){
        Msg msg = currMsg.top().second;
        currMsg.pop();
        if(msg.src != key && msg.dstn!=key)
            graph.NodeList[msg.src]->pushDataCurrMsg(msg);
    }
    currMsgLock.unlock();
    arrivingMsgLock.lock();
    while(!arrivingMsg.empty()){
        Msg msg = arrivingMsg.top().second;
        arrivingMsg.pop();
        if(msg.src!=key && msg.dstn!=key)
            graph.NodeList[msg.src]->pushDataCurrMsg(msg);
    }
    arrivingMsgLock.unlock();
}
/***********************      NODE CLASS FUNCTIONS(END)  *******************************/




/***********************      GRAPH CLASS FUNCTIONS      *******************************/
/* Name : readNextHop
 *
 * Arguments :
 *      p       -> message priority for which routing table is to be looked up
 *      src     -> message source
 *      dsnt    -> message destination
 *
 *  Description :
 *      Read nextHop matrix to find the next hop for a given msg priority, src & dstn. Acquire lock before reading
 *
 *  Returns :
 *      character representing the next hop node for this msg
 */
char Graph::readNextHop(Priority p, char src, char dstn){
    nextHopLock.lock();
    char x = nextHop[p][src][dstn];
    nextHopLock.unlock();
    return x;
}


/* Name : setNextHopMatrix
 *
 * Arguments :
 *      nextHopLocal -> matrix containing next hop data to be copied to global nextHop matrix
 *
 * Description :
 *      Copy the contents of next hop matrix passed as argument to global nextHop matrix data structure. Acquire lock
 *      to ensure thread safety
 *
 * Returns : None
 */
void Graph::setNextHopMatrix(map<Priority , map<char, map<char, char>>>& nextHopLocal){
    nextHopLock.lock();
    nextHop = nextHopLocal;
    nextHopLock.unlock();
}
/***********************      GRAPH CLASS FUNCTIONS (END)  *****************************/




/***************************************************************************************/
/* Name : computeRoutingTable
 *
 * Arguments : None
 *
 * Description :
 *      Starting point for thread which computes routing table every 10 seconds. It runs iteratively and applies
 *      Dijkstra over all the vertices and computes nextHop matrix for each possible src, dstn, msg priority.
 *
 * Returns : None but sets global data structure nextHop matrix
 */
void computeRoutingTable(){
    chrono::duration<int> ten_sec(10);
    while (!Exit) {
        graph.NodeListLock.lock();
        debug << " compute routing table" << endl;
        Utils::displayGraphData();
        map<Priority, map<char, map<char, char>>> nextHopLocal;
        map<char, char> parentNode;
        for (int i = 0; i <= 2; ++i) {
            for (map<char, Node *>::iterator it = graph.NodeList.begin(); it != graph.NodeList.end(); ++it) {
                char src = it->first;
                Utils::Dijkstra(src, (Priority) i, parentNode);
                for (map<char, char>::iterator it2 = parentNode.begin(); it2 != parentNode.end(); ++it2) {
                    char dstn = it2->first;
                    nextHopLocal[(Priority) i][src][dstn] = Utils::getParent(src, dstn, parentNode);
                }
            }
        }
        Utils::displayNextHopData(nextHopLocal);
        graph.setNextHopMatrix(nextHopLocal);
        graph.NodeListLock.unlock();
        if (!ready) ready = true;
        this_thread::sleep_for(ten_sec);
    }
}

/* Name : NodeProcessing
 *
 * Arguments :
 *      node    -> A pointer to a Data Center instance / Node.
 *
 * Description :
 *      Starting point for thread corresponding to each Data Center. Runs iteratively every 1 second and does the
 *      necessary processing for arrivingMsg queue and currMsg queue until terminated
 *
 * Returns : None
 */
void NodeProcessing(Node* node){
    chrono::duration<int> one_sec(1);
    while(!ready)
        this_thread::sleep_for(one_sec);
    try {
        while (!Exit & !node->terminate) {
            node->processArrivedMsges();
            node->processCurrMsg();
            //Make the thread sleep for one second
            this_thread::sleep_for(one_sec);
        }
    } catch (...) {
        debug << "WORKER THREAD CRASHED" << endl;
    }
}

/* Name : readInput
 *
 * Arguments : None
 *
 * Description :
 *      A utility fn to read graph data initially from input.txt file. It initializes the graph instance with data read
 *      from input.txt file, computes initial routing table and then starts thread corresponding to each thread
 */
void readInput(){
    ifstream input("input.txt");
    int V, E, wt; char key, key_neigh, pri; Priority p;
    input >> V;
    for(int ctr=0; ctr<V; ++ctr) {
        input >> key; // do some error checking here
        if (graph.NodeList.find(key) == graph.NodeList.end()) {
            Utils::createNewNode(key);
        }
        input >> E;
        for (int i = 0; i < E; ++i) {
            input >> key_neigh >> wt;
            if (graph.NodeList.find(key_neigh) == graph.NodeList.end())
                Utils::createNewNode(key_neigh);
            graph.NodeList[key]->neighbors[key_neigh] = wt;
            graph.NodeList[key_neigh]->neighbors[key] = wt;
        }
        input >> pri;
        p = Utils::convertCharToPriority(pri);
        graph.NodeList[key]->pri = p;
    }

    routingTableThread = thread(computeRoutingTable);
}

/* Name : addNewNode
 *
 * Arguments :
 *      key     -> key of new Data Center coming up
 *
 * Description :
 *      Handler function for adding a new Node to graph with specified key. From console input, read neighbors &
 *      initialize its neighbors. Update neighbor information for its neighboring nodes as well. If the key is already
 *      present in graph don't do anything
 */
void addNewNode(char key){
    vector<pair<int, char>> edges;
    int E, wt; char dstn, p; Priority  pri;
    cin >> E;
    while(E--){
        cin >> dstn >> wt;
        edges.push_back(make_pair(wt, dstn));
    }
    cin >> p; pri = Utils::convertCharToPriority(p);
    if (T.find(key)!=T.end()){
        cout << "NODE " << key << " ALREADY PRESENT. Returning \n";
        return;
    }
    debug << "read data successfully" << endl;
    graph.NodeListLock.lock();
    Node *node = new Node(key);
    while(!edges.empty()){
        node->neighbors[edges[0].second] = edges[0].first;
        graph.NodeList[edges[0].second]->neighbors[key] = edges[0].first;
        edges.erase(edges.begin());
    }
    node->pri = pri;
    Node* tmpnode;
    // If key already present in graph don't do anything
    if(graph.NodeList.find(key)!=graph.NodeList.end()) {
        tmpnode = graph.NodeList[key];
        graph.NodeList[key] = node;
        delete tmpnode;
    } else
        graph.NodeList[key] = node;
    // start a thread for this node
    T[key] = thread(NodeProcessing, node);
    graph.NodeListLock.unlock();
    debug << "graph modified successully" <<endl;
}

/* Name : deleteNode
 *
 * Argument :
 *      key     -> key of the Data Center to be killed
 *
 * Description :
 *      Handler function for delete Node functionality. Kill thread for this node/data center, then clear up its data
 *      structures - arrivingMsg and currMsg queue. Subsequently, delete this node from graph
 *
 * Returns : None
 */
void deleteNode(char key) {
    graph.NodeListLock.lock();
    Node *node = graph.NodeList[key];
    node->terminate = true;
    if(T[key].joinable()) T[key].join();
    T.erase(key);
    node->deleteAllQueues();
    graph.NodeList.erase(key);
    delete node;
    graph.NodeListLock.unlock();
}

/* Name : addNewEdge
 *
 * Argument :
 *      src     -> source key
 *      dsnt    -> dstn key
 *      wt      -> weight of the edge
 *
 * Description :
 *      Handler function to add a new edge between existing data centers. Update the neighbor information of both nodes
 *
 * Returns : None
 */
void addNewEdge(char src, char dstn, int wt) {
    if (graph.NodeList.find(src)==graph.NodeList.end() || graph.NodeList.find(dstn)==graph.NodeList.end()){
        output << "Either " << src << " or " << dstn << " not present in graph" << endl;
        output << "UNABLE TO ADD EDGE" << endl;
        return ;
    }
    graph.NodeListLock.lock();
    graph.NodeList[src]->neighbors[dstn] = wt;
    graph.NodeList[dstn]->neighbors[src] = wt;
    graph.NodeListLock.unlock();
}

/* Name : main()
 *
 * Arguments : None
 *
 * Description :
 *      Main function of the program. Reads initial graph data from input.txt and then iteratively waits for user action
 *      It practically represents a thread handling User requests - UI for this program
 *
 * Returns : return code of this program
 */
int main() {
    ifstream inputmsg("msg.txt");
    readInput();
    int option, n;  char src, dstn, p, ch; Priority pri;
    Msg msg;
    while (true){
        cout << "Please select an option by entering integer value corresponding to it : \n";
        cout << "1 - Read msges from msg.txt\n";
        cout << "2 - Kill a node\n";
        cout << "3 - Add a node\n";
        cout << "4 - Add an edge (src, dstn, wt)\n";
        cout << "5 - Exit\n";
        cout << "Enter option no. and press enter : ";
        cin >> option;
        switch (option){
            case 1:
                inputmsg >> n;
                while(n--) {
                    inputmsg >> src >> dstn >> p;
                    pri = Utils::convertCharToPriority(p);
                    msg = Msg(src, dstn, pri);
                    graph.NodeList[src]->pushDataCurrMsg(msg);
                }
                break;
            case 2:
                cout << "Enter node key to delete : ";
                cin >> ch;
                deleteNode(ch);
                break;
            case 3:
                cout << "Enter node + edges in same format as in input.txt : ";
                cin >> ch;
                addNewNode(ch);
                break;
            case 4:
                cout << "Enter edge - src, dstn, wt : ";
                cin >> src >> dstn >> n;
                addNewEdge(src, dstn, n);
            default : break;
        }
        if(option==5) break;
    }
    Exit = true;
    for(auto& th:T) {
        if (th.second.joinable()) th.second.join();
    }
    routingTableThread.join();
    return 0;
}



/*************      UTILITY FUNCTIONS       *******************/

/* Name : createNewNode
 *
 * Arguments :
 *      key     -> Data center key for which a new node has to be created
 *
 * Description :
 *      Create a new Node/Data Center using this key. Also, start a new thread for this node. Add this node to the
 *      graph
 *
 * Returns : None
 */
void Utils::createNewNode(char key){
    Node* node = new Node(key);
    T[key] = thread(NodeProcessing, node);
    graph.NodeList[key] = node;
}


/* Name : convertCharToPriority
 *
 * Arguments :
 *      x       -> character representing priority of msg/Data Center
 *
 * Description :
 *      Convert the character to appropriate value of Priority enum (LOW, MEDIUM, HIGH)
 *
 * Returns :
 *      A member of enum Priority representing msg/Data center priority
 */
Priority Utils::convertCharToPriority(char x){
    switch(x){
        case 'L': return LOW;
        case 'M': return MEDIUM;
        case 'H': return HIGH;
        default :
            cout << "INVALID PRIORITY ENTERED, RETURNING LOW";
            return LOW;
    }
}

/* Name : getEdgeWt
 *
 * Argument :
 *      it      -> iterator to a key value pair in map<char, int> neighbors
 *      pri_msg -> priority of the message to be sent on the edge
 *
 * Description :
 *      Compute the effective edge weight by taking into account the load at neighbor. Calculates the
 *      time it will take for a message to be processed at the neighboring node. Adds it to the edge weight to
 *      get the effective edge weight.
 *
 * Returns :
 *      An integer representing the edge weight between two nodes
 */
int Utils::getEdgeWt(map<char, int>::iterator it, Priority pri_msg){
    int delay = 0;
    char key_neigh = it->first;
    Priority pri_neigh = graph.NodeList[key_neigh]->pri;
    int edgeWt = it->second + dcProccessTime[pri_neigh][pri_msg];
    delay = graph.NodeList[key_neigh]->getCurrentDelay(pri_msg);
    edgeWt += delay;
    return edgeWt;
}

/* Name : isNextHopValid
 *
 * Arguments :
 *      ch      -> character representing a Data Center key
 *
 * Description :
 *      Checks whether the key(or Node) is present in graph or not
 *
 * Returns :
 *      True if key present in graph (Data Center exits) else False
 */
bool Utils::isNextHopValid(char ch){
    return !(graph.NodeList.find(ch) == graph.NodeList.end());
}


/* Name : displayNextHopData
 *
 * Arguments :
 *      nextHopLocal    -> next hop matrix to be displayed
 *
 * Description :
 *      A debug utility function to print the contents of the next hop matrix passed to it as argument to debug file
 *
 * Returns : None
 */
void Utils::displayNextHopData(map<Priority , map<char, map<char, char>>> nextHopLocal){
    for(int i=0; i<=2; i++){
        Priority  pri = (Priority) i;
        debug << "Next Hop Matrix for Priority of Msg : " << pri << endl;
        for(map<char, map<char,char>>::iterator it=nextHopLocal[pri].begin(); it!=nextHopLocal[pri].end(); ++it){
            debug << it->first << " : " ;
            for(map<char,char>::iterator it2=nextHopLocal[pri][it->first].begin(); it2!=nextHopLocal[pri][it->first].end(); ++it2)
                debug << it2->second << " ";
            debug << endl;
        }
        debug << endl ;
    }
}

/* Name : displayMsg
 *
 * Arguments :
 *      msg     -> a message instance
 *
 * Description :
 *      A utility function to display the contents of msg passed as argument with proper formatting
 *
 * Returns : None
 */
void Utils::displayMsg(Msg msg){
    output << "Src:" << msg.src << " Dstn:" << msg.dstn << " Pri:" << msg.pri << " Path : ";
    for(int i=0; i<msg.path.size(); i++){
        output << msg.path[i] << " ";
    }
    output << "\n\tInt:" << ctime(&msg.intime) << "\tOut:" \
        << ctime(&msg.outtime) << " Total:" << (msg.outtime-msg.intime) << endl << endl;
}

/* Name : displayGraphData
 *
 * Arguments : None
 *
 * Description :
 *      A debug utility function to display current contents of graph. Prints all the Data Centers (nodes) in the
 *      graph, along with their neighbors and their priorities
 *
 * Returns : None
 */
void Utils::displayGraphData() {
    for(map<char, Node*>::iterator it=graph.NodeList.begin(); it!=graph.NodeList.end(); ++it){
        debug << it->first << "(" << it->second->pri << ")" << ": ";
        for(map<char,int>::iterator it2=it->second->neighbors.begin(); it2!=it->second->neighbors.end(); ++it2)
            debug << it2->first << " ";
        debug << endl;
    }
    debug << endl;
}

// A utility function for computing the next hop table
char Utils::getParent(char src, char dstn, map<char, char>& parentNode){
    if(src == dstn) return '-';
    if(parentNode[dstn]==0) return '*';
    if(parentNode[dstn] == src) return dstn;
    return getParent(src, parentNode[dstn], parentNode);
}

/* Name : Dijkstra
 *
 * Arguments :
 *      src     -> Src key
 *      pri_msg -> priority of the message for which the shortest path is to be calculated
 *      parentNode -> A utility list used to retrace shortest path between src and any other node of graph
 *
 * Description :
 *      Compute the shortest path between the source and all other nodes of the graph using Dijkstra algorithm. Takes
 *      into account dynamic/effective edge weight and not the static values
 *
 * Returns : parentNode list passed as reference
 */
void Utils::Dijkstra(char src, Priority pri_msg, map<char, char>& parentNode){
    map<char, bool> isVisited;
    map <char, int> distance;
    char key;
    for(map<char,Node*>::iterator it=graph.NodeList.begin(); it!=graph.NodeList.end(); ++it){
        key = it->first;
        distance[key] = INT_MAX;
        isVisited[key] = false;
        parentNode[key] = 0;
    }
    distance[src] = 0;
    // first = distance from src, second = node key of neighbor
    priority_queue<pair<int, char>> pq; //write Compare function here
    pq.push(make_pair(0, src));
    while(!pq.empty()){
        pair<int, char> top = pq.top();
        pq.pop();
        char currNodeKey = top.second;
        if(isVisited[currNodeKey]) continue;
        isVisited[currNodeKey] = true;
        //iterate over all neighbors
        for(map<char, int>::iterator it=graph.NodeList[currNodeKey]->neighbors.begin(); it!=graph.NodeList[currNodeKey]->neighbors.end(); ++it){
            int edgeWt = Utils::getEdgeWt(it, pri_msg);
            char key_neigh = it->first;
            if(distance[currNodeKey]!=INT_MAX && distance[currNodeKey]+edgeWt < distance[key_neigh]){
                distance[key_neigh] = distance[currNodeKey] + edgeWt;
                parentNode[key_neigh] = currNodeKey;
                pq.push(make_pair(distance[key_neigh], key_neigh));
            }
        }
    }
}
/*************      UTILITY FUNCTIONS(END)  ****************/