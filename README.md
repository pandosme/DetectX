python train.py --img 800 --batch 50 --epochs 300 --data ~/dataset/backyard/yolo/data.yaml --weights yolov5n.pt --cfg yolov5n.yaml
python export.py --weights runs/train/exp/weights/best.pt --include tflite --int8 --per-tensor --img-size 800
0.003921568859368563