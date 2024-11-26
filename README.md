# Data Storage Service with Load Balancing

This project involves developing a data storage service that receives data from clients. The service consists of two main components:

1. **Load Balancer (LB)**
2. **Worker (WR)**

## Components and Their Functions

### 1. Load Balancer (LB)
- Listens for incoming requests on port 5059 and handles storage requests from clients.
- Directs each request to the Worker (WR) component that currently has the least amount of stored data.
- Receives information about available WRs, as they automatically register themselves upon startup and deregister upon shutdown.
- When a new WR is added to the system, the LB redistributes data so that all WR components have the same data.

### 2. Worker (WR)
- Upon receiving data from the LB, stores the data in its local storage and sends notifications to all other WR components about the newly received data.
- Based on the notifications received from other WRs, each WR updates its local storage to ensure it has a complete set of all the data ever received by the system.
- Informs the LB once the data is successfully stored.

## How the System Works

- **Data Storage**: Clients send storage requests to the LB. The LB then distributes the requests to the WRs based on the load balancing algorithm.
- **Synchronization**: WRs continuously synchronize their data with each other by sending notifications about received data. This ensures that each WR has a complete copy of all the data that has been stored.
- **Scaling**: When a new WR is added to the system, the LB redistributes the data so that the load is balanced across all WRs. This ensures uniformity in the data storage across the system.


## Conclusion

This service provides an efficient and scalable solution for distributed data storage, ensuring that data is evenly distributed across workers and is always synchronized across all components.
