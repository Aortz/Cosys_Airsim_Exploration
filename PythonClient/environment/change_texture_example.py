import cosysairsim as airsim

c = airsim.MultirotorClient()
c.confirmConnection()

c.simSetObjectMaterialFromTexture("OrangeBall", "sample_texture.jpg")

