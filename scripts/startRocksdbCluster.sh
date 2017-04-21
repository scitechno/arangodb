#!/bin/bash

. `dirname $0`/cluster-run-common.sh

if [ "$POOLSZ" == "" ] ; then
  POOLSZ=$NRAGENTS
fi


printf "Starting agency ... \n"
printf "  # agents: %s," "$NRAGENTS"
printf " # db servers: %s," "$NRDBSERVERS"
printf " # coordinators: %s," "$NRCOORDINATORS"
printf " transport: %s\n" "$TRANSPORT"

if [ ! -d arangod ] || [ ! -d arangosh ] || [ ! -d UnitTests ] ; then
  echo Must be started in the main ArangoDB source directory.
  exit 1
fi

if [[ $(( $NRAGENTS % 2 )) == 0 ]]; then
  echo "**ERROR: Number of agents must be odd! Bailing out."
  exit 1
fi

if [ ! -d arangod ] || [ ! -d arangosh ] || [ ! -d UnitTests ] ; then
    echo "Must be started in the main ArangoDB source directory! Bailing out."
    exit 1
fi

if [ ! -z "$INTERACTIVE_MODE" ] ; then
    if [ "$INTERACTIVE_MODE" == "C" ] ; then
        COORDINATORCONSOLE=1
        echo "Starting one coordinator in terminal with --console"
    elif [ "$INTERACTIVE_MODE" == "D" ] ; then
        CLUSTERDEBUGGER=1
        echo Running cluster in debugger.
    elif [ "$INTERACTIVE_MODE" == "R" ] ; then
        RRDEBUGGER=1
        echo Running cluster in rr with --console.
    fi
fi

SFRE=5.0
COMP=200000
KEEP=500
MINT=0.2
MAXT=1.0
AG_BASE=$(( $PORT_OFFSET + 4001 ))
CO_BASE=$(( $PORT_OFFSET + 8530 ))
DB_BASE=$(( $PORT_OFFSET + 8629 ))
NATH=$(( $NRDBSERVERS + $NRCOORDINATORS + $NRAGENTS ))

rm -rf cluster
if [ -d cluster-init ];then
  cp -a cluster-init cluster
fi
mkdir -p cluster

if [ -z "$JWT_SECRET" ];then
  AUTHENTICATION="--server.authentication false"
  AUTHORIZATION_HEADER=""
else
  AUTHENTICATION="--server.jwt-secret $JWT_SECRET"
  AUTHORIZATION_HEADER="Authorization: bearer $(jwtgen -a HS256 -s $JWT_SECRET -c 'iss=arangodb' -c 'preferred_username=root')"
fi

if [ "$TRANSPORT" == "ssl" ]; then
  SSLKEYFILE="--ssl.keyfile UnitTests/server.pem"
  CURL="curl --insecure $CURL_AUTHENTICATION -s -f -X GET https:"
else
  SSLKEYFILE=""
  CURL="curl -s -f $CURL_AUTHENTICATION -X GET http:"
fi

echo Starting agency ... 
for aid in `seq 0 $(( $NRAGENTS - 1 ))`; do
    port=$(( $AG_BASE + $aid ))
    AGENCY_ENDPOINTS+="--cluster.agency-endpoint $TRANSPORT://localhost:$port "
    ${BUILD}/bin/arangod \
        -c none \
        --agency.activate true \
        --agency.compaction-step-size $COMP \
        --agency.compaction-keep-size $KEEP \
        --agency.election-timeout-min $MINT \
        --agency.election-timeout-max $MAXT \
        --agency.endpoint $TRANSPORT://localhost:$AG_BASE \
        --agency.my-address $TRANSPORT://localhost:$port \
        --agency.pool-size $NRAGENTS \
        --agency.size $NRAGENTS \
        --agency.supervision true \
        --agency.supervision-frequency $SFRE \
        --agency.supervision-grace-period 15 \
        --agency.wait-for-sync false \
        --database.directory cluster/data$port \
        --javascript.app-path ./js/apps \
        --javascript.startup-directory ./js \
        --javascript.module-directory ./enterprise/js \
        --javascript.v8-contexts 1 \
        --server.endpoint $TRANSPORT://0.0.0.0:$port \
        --server.statistics false \
        --server.threads 16 \
        --log.file cluster/$port.log \
        --log.force-direct true \
        --log.level agency=$LOG_LEVEL_AGENCY \
        --cluster.system-replication-factor 1 \
	--server.storage-engine rocksdb \
        $AUTHENTICATION \
        $SSLKEYFILE \
        > cluster/$port.stdout 2>&1 &
done

start() {
    if [ "$1" == "dbserver" ]; then
        ROLE="PRIMARY"
    elif [ "$1" == "coordinator" ]; then
        ROLE="COORDINATOR"
    fi
    TYPE=$1
    PORT=$2
    mkdir cluster/data$PORT
    echo Starting $TYPE on port $PORT
    mkdir -p cluster/apps$PORT 
    ${BUILD}/bin/arangod \
       -c none \
       --database.directory cluster/data$PORT \
       --cluster.agency-endpoint $TRANSPORT://127.0.0.1:$AG_BASE \
       --cluster.my-address $TRANSPORT://127.0.0.1:$PORT \
       --server.endpoint $TRANSPORT://0.0.0.0:$PORT \
       --cluster.my-local-info $TYPE:127.0.0.1:$PORT \
       --server.endpoint $TRANSPORT://0.0.0.0:$PORT \
       --cluster.my-role $ROLE \
       --log.file cluster/$PORT.log \
       --log.level $LOG_LEVEL \
       --server.statistics true \
       --server.threads 5 \
       --javascript.startup-directory ./js \
       --javascript.module-directory ./enterprise/js \
       --javascript.app-path cluster/apps$PORT \
       --log.force-direct true \
       --log.level cluster=$LOG_LEVEL_CLUSTER \
       --cluster.system-replication-factor 1 \
       --server.storage-engine rocksdb \
        $AUTHENTICATION \
        $SSLKEYFILE \
       > cluster/$PORT.stdout 2>&1 &
}

startTerminal() {
    if [ "$1" == "dbserver" ]; then
      ROLE="PRIMARY"
    elif [ "$1" == "coordinator" ]; then
      ROLE="COORDINATOR"
    fi
    TYPE=$1
    PORT=$2
    mkdir cluster/data$PORT
    echo Starting $TYPE on port $PORT
    $XTERM $XTERMOPTIONS -e "${BUILD}/bin/arangod \
        -c none \
        --database.directory cluster/data$PORT \
        --cluster.agency-endpoint $TRANSPORT://127.0.0.1:$AG_BASE \
        --cluster.my-address $TRANSPORT://127.0.0.1:$PORT \
        --server.endpoint $TRANSPORT://0.0.0.0:$PORT \
        --cluster.my-role $ROLE \
        --log.file cluster/$PORT.log \
        --log.level $LOG_LEVEL \
        --server.statistics true \
        --server.threads 5 \
        --javascript.startup-directory ./js \
        --javascript.module-directory ./enterprise/js \
        --javascript.app-path ./js/apps \
        $AUTHENTICATION \
        $SSLKEYFILE \
        --console" &
}

startDebugger() {
    if [ "$1" == "dbserver" ]; then
        ROLE="PRIMARY"
    elif [ "$1" == "coordinator" ]; then
        ROLE="COORDINATOR"
    fi
    TYPE=$1
    PORT=$2
    mkdir cluster/data$PORT
    echo Starting $TYPE on port $PORT with debugger
    ${BUILD}/bin/arangod \
      -c none \
      --database.directory cluster/data$PORT \
      --cluster.agency-endpoint $TRANSPORT://127.0.0.1:$AG_BASE \
      --cluster.my-address $TRANSPORT://127.0.0.1:$PORT \
      --server.endpoint $TRANSPORT://0.0.0.0:$PORT \
      --cluster.my-role $ROLE \
      --log.file cluster/$PORT.log \
      --log.level $LOG_LEVEL \
      --server.statistics false \
      --server.threads 5 \
      --javascript.startup-directory ./js \
      --javascript.module-directory ./enterprise/js \
      --javascript.app-path ./js/apps \
      --cluster.system-replication-factor 1 \
      --server.storage-engine rocksdb \
      $SSLKEYFILE \
      $AUTHENTICATION &
      $XTERM $XTERMOPTIONS -e "gdb ${BUILD}/bin/arangod -p $!" &
}

startRR() {
    if [ "$1" == "dbserver" ]; then
        ROLE="PRIMARY"
    elif [ "$1" == "coordinator" ]; then
        ROLE="COORDINATOR"
    fi
    TYPE=$1
    PORT=$2
    mkdir cluster/data$PORT
    echo Starting $TYPE on port $PORT with rr tracer
    $XTERM $XTERMOPTIONS -e "rr ${BUILD}/bin/arangod \
        -c none \
        --database.directory cluster/data$PORT \
        --cluster.agency-endpoint $TRANSPORT://127.0.0.1:$AG_BASE \
        --cluster.my-address $TRANSPORT://127.0.0.1:$PORT \
        --server.endpoint $TRANSPORT://0.0.0.0:$PORT \
        --cluster.my-role $ROLE \
        --log.file cluster/$PORT.log \
        --log.level $LOG_LEVEL \
        --server.statistics true \
        --server.threads 5 \
        --javascript.startup-directory ./js \
        --javascript.module-directory ./enterprise/js \
        --javascript.app-path ./js/apps \
        --cluster.system-replication-factor 1 \
        --server.storage-engine rocksdb \        
        $AUTHENTICATION \
        $SSLKEYFILE \
        --console" &
}

PORTTOPDB=`expr $DB_BASE + $NRDBSERVERS - 1`
for p in `seq $DB_BASE $PORTTOPDB` ; do
    if [ "$CLUSTERDEBUGGER" == "1" ] ; then
        startDebugger dbserver $p
    elif [ "$RRDEBUGGER" == "1" ] ; then
        startRR dbserver $p
    else
        start dbserver $p
    fi
done

if [ "$NRCOORDINATORS" -gt 0 ] ; then
PORTTOPCO=`expr $CO_BASE + $NRCOORDINATORS - 1`
for p in `seq $CO_BASE $PORTTOPCO` ; do
    if [ "$CLUSTERDEBUGGER" == "1" ] ; then
        startDebugger coordinator $p
    elif [ $p == "$CO_BASE" -a ! -z "$COORDINATORCONSOLE" ] ; then
        startTerminal coordinator $p
    elif [ "$RRDEBUGGER" == "1" ] ; then
        startRR coordinator $p
    else
        start coordinator $p
    fi
done
fi

if [ "$CLUSTERDEBUGGER" == "1" ] ; then
    echo Waiting for you to setup debugger windows, hit RETURN to continue!
    read
fi

echo Waiting for cluster to come up...

testServer() {
    PORT=$1
    while true ; do
        if [ -z "$AUTHORIZATION_HEADER" ]; then
          ${CURL}//127.0.0.1:$PORT/_api/version > /dev/null 2>&1
        else
          ${CURL}//127.0.0.1:$PORT/_api/version -H "$AUTHORIZATION_HEADER" > /dev/null 2>&1
        fi
        if [ "$?" != "0" ] ; then
            echo Server on port $PORT does not answer yet.
        else
            echo Server on port $PORT is ready for business.
            break
        fi
        sleep 1
    done
}

for p in `seq $DB_BASE $PORTTOPDB` ; do
    testServer $p
done

if [ "$NRCOORDINATORS" -gt 0 ] ; then
for p in `seq $CO_BASE $PORTTOPCO` ; do
    testServer $p
done
fi

if [ "$SECONDARIES" == "1" ] ; then
    let index=1
    PORTTOPSE=`expr 8729 + $NRDBSERVERS - 1` 
    for PORT in `seq 8729 $PORTTOPSE` ; do
        let dbserverindex=$index-1
        mkdir cluster/data$PORT
        
        CLUSTER_ID="Secondary$index"
        
        DBSERVER_ID=$(curl -s 127.0.0.1:$CO_BASE/_admin/cluster/health | jq '.Health | to_entries | map(select(.value.Role == "DBServer")) | .' | jq -r ".[$dbserverindex].key")
        echo Registering secondary $CLUSTER_ID for $DBSERVER_ID
        curl -s -f -X PUT --data "{\"primary\": \"$DBSERVER_ID\", \"oldSecondary\": \"none\", \"newSecondary\": \"$CLUSTER_ID\"}" -H "Content-Type: application/json" localhost:$CO_BASE/_admin/cluster/replaceSecondary
        echo Starting Secondary $CLUSTER_ID on port $PORT
        ${BUILD}/bin/arangod \
            -c none \
            --database.directory cluster/data$PORT \
            --cluster.agency-endpoint $TRANSPORT://127.0.0.1:$AG_BASE \
            --cluster.my-address $TRANSPORT://127.0.0.1:$PORT \
            --server.endpoint $TRANSPORT://0.0.0.0:$PORT \
            --cluster.my-id $CLUSTER_ID \
            --log.file cluster/$PORT.log \
            --server.statistics true \
            --javascript.startup-directory ./js \
            --javascript.module-directory ./enterprise/js \
            --cluster.system-replication-factor 1 \
            --server.storage-engine rocksdb \
            $AUTHENTICATION \
            $SSLKEYFILE \
            --javascript.app-path ./js/apps \
            > cluster/$PORT.stdout 2>&1 &
            
            let index=$index+1
    done
fi

echo Done, your cluster is ready at
if [ "$NRCOORDINATORS" -gt 0 ] ; then
for p in `seq $CO_BASE $PORTTOPCO` ; do
    echo "   ${BUILD}/bin/arangosh --server.endpoint $TRANSPORT://127.0.0.1:$p"
done
fi
